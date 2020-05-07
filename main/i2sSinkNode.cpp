#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2s.h"
#include "driver/dac.h"
#include "esp_log.h"
#include "esp_err.h"
#include "i2sSinkNode.hpp"

void I2sOutputNode::adjustSamplesForInternalDac(char* sBuff, int len)
{
    int16_t* buf16 = (int16_t*)sBuff;
    auto end = buf16 + len / 2;
    for(; buf16 < end; buf16++) {
        (*buf16) &= 0xff00;
        (*buf16) += 0x8000;//turn signed value into unsigned, expand negative value into positive range
    }
}

template <typename T>
void I2sOutputNode::applyVolume(DataPullReq& dpr)
{
    T* pSample = (T*)dpr.buf;
    T* end = pSample + dpr.size / sizeof(T);
    for(; pSample < end; pSample++) {
        *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) / kVolumeDiv;
    }
}

void I2sOutputNode::nodeThreadFunc()
{
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            DataPullReq dpr(10240); // read all available data
#ifndef NDEBUG
            auto tim = esp_timer_get_time();
#endif
            auto err = mPrev->pullData(dpr, mReadTimeout);
#ifndef NDEBUG
            tim = (esp_timer_get_time() - tim) / 1000;
            if (err == kTimeout) {
                if (abs(tim - mReadTimeout) > portTICK_PERIOD_MS) {
                    ESP_LOGW(mTag, "pullData timed out with unexpected delay: took %lld ms, timeout was %d ms", tim, mReadTimeout);
                }
            } else if (err == kNoError) {
                if (tim - mReadTimeout > 5) {
                    ESP_LOGW(mTag, "pullData returned data but took more than specified by timeout: took %lld ms, timeout was %d ms", tim, mReadTimeout);
                }
            }
#endif
            if (err == kTimeout || err == kStreamFlush) {
                ESP_LOGW(mTag, "Read timeout, sending silence");
                i2s_zero_dma_buffer(mPort);
                continue;
            } else if (err) {
                i2s_zero_dma_buffer(mPort);
                setState(kStatePaused);
                break;
            }
            if (dpr.fmt != mFormat) {
                setFormat(dpr.fmt);
            }

            if (mVolume != kVolumeDiv) {
                applyVolume<int16_t>(dpr);
            }

            if (mUseInternalDac) {
                adjustSamplesForInternalDac(dpr.buf, dpr.size);
            }
            size_t written;
            auto espErr = i2s_write(mPort, dpr.buf, dpr.size, &written, portMAX_DELAY);
            mPrev->confirmRead(dpr.size);
            if (espErr != ESP_OK) {
                ESP_LOGE(mTag, "i2s_write error: %s", esp_err_to_name(espErr));
                continue;
            }
            if (written != dpr.size) {
                ESP_LOGE(mTag, "is2_write() wrote less than requested with infinite timeout");
            }
        }
    }
}
void I2sOutputNode::dmaFillWithSilence()
{
    enum { kSampleCnt = 64 };
    uint16_t buf[kSampleCnt];
    auto end = buf + kSampleCnt;
    uint16_t val = mUseInternalDac ? 0x8000 : 0x0000;
    for (uint16_t* ptr = buf; ptr < end; ptr++) {
        *ptr = val;
    }
    size_t written = 0;
    do {
        i2s_write(mPort, buf, 128, &written, 0);
    } while (written == 128);
}

bool I2sOutputNode::setFormat(StreamFormat fmt)
{
    auto bits = fmt.bits();
    if (bits != 16) {
        ESP_LOGE(mTag, "Only 16bit sample width is supported, but %d provided", bits);
        return false;
    }
    auto samplerate = fmt.samplerate;
    ESP_LOGW(mTag, "Setting output mode to %d-bit %s, %d Hz", bits,
        (fmt.channels() == 2) ? "stereo" : "mono", samplerate);
    auto err = i2s_set_clk(mPort, samplerate,
        (i2s_bits_per_sample_t)fmt.bits(), (i2s_channel_t)fmt.channels());
    if (err == ESP_FAIL) {
        ESP_LOGE(mTag, "i2s_set_clk failed: rate: %d, bits: %d, ch: %d. Error: %s",
            samplerate, bits, fmt.channels(), esp_err_to_name(err));
        return false;
    }
    mFormat = fmt;
    recalcReadTimeout(samplerate);
    return true;
}

void I2sOutputNode::recalcReadTimeout(int samplerate)
{
    mReadTimeout = 1000 * (kDmaBufCnt * kDmaBufLen) / samplerate;
    ESP_LOGW(mTag, "Setting read timeout to %d ms", mReadTimeout);
}

I2sOutputNode::I2sOutputNode(int port, i2s_pin_config_t* pinCfg)
:AudioNodeWithTask("i2s-out", kStackSize, 16), mFormat(kDefaultSamplerate, 16, 2)
{
    if (port == 0xff) {
        mUseInternalDac = true;
        mPort = I2S_NUM_0;
    } else {
        mUseInternalDac = false;
        mPort = (i2s_port_t)port;
    }
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = kDefaultSamplerate;
    cfg.bits_per_sample = (i2s_bits_per_sample_t) 16;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_I2S_MSB;
    cfg.dma_buf_count = kDmaBufCnt;
    cfg.dma_buf_len = kDmaBufLen;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL2;
    cfg.tx_desc_auto_clear = true;

    if (mUseInternalDac) {
        cfg.mode = (i2s_mode_t)(cfg.mode | I2S_MODE_DAC_BUILT_IN);
    }

    auto err = i2s_driver_install(mPort, &cfg, 0, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(mTag, "Error installing i2s driver: %s", esp_err_to_name(err));
        myassert(false);
    }

    if (mUseInternalDac) {
        i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);
    } else {
        myassert(pinCfg);
        i2s_set_pin(mPort, pinCfg);
    }
//  i2s_mclk_gpio_select(i2s->config.i2s_port, GPIO_NUM_0);
    i2s_zero_dma_buffer(mPort);
    recalcReadTimeout(kDefaultSamplerate);
}

I2sOutputNode::~I2sOutputNode()
{
    i2s_driver_uninstall(mPort);
}

uint16_t I2sOutputNode::getVolume() const
{
    return (mVolume * 100 + kVolumeDiv/2) / kVolumeDiv;
}

void I2sOutputNode::setVolume(uint16_t vol)
{
    mVolume = ((vol * kVolumeDiv + 50) / 100);
    ESP_LOGW(mTag, "setVolume(%u) -> %u", vol, mVolume);
}
