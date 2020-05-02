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

static const char *TAG = "I2S_SINK";

void I2sSinkNode::adjustSamplesForInternalDac(char* sBuff, int len)
{
    int16_t* buf16 = (int16_t*)sBuff;
    auto end = buf16 + len / 2;
    for(; buf16 < end; buf16++) {
        (*buf16) &= 0xff00;
        (*buf16) += 0x8000;//turn signed value into unsigned, expand negative value into positive range
    }
}

void I2sSinkNode::nodeThreadFunc()
{
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            DataPullReq dpr(10240); // read all available data
            auto err = mPrev->pullData(dpr, 1000);
            if (err == kTimeout) {
                ESP_LOGW(mTag, "Read timeout, sending silence");
                dmaFillWithSilence();
                continue;
            } else if (err) {
                i2s_zero_dma_buffer(mPort);
                setState(kStatePaused);
                break;
            }
            if (dpr.fmt != mFormat) {
                setFormat(dpr.fmt);
            }
            if (mUseInternalDac) {
                adjustSamplesForInternalDac(dpr.buf, dpr.size);
            }
            size_t written;
            auto espErr = i2s_write(mPort, dpr.buf, dpr.size, &written, portMAX_DELAY);
            mPrev->confirmRead(dpr.size);
            if (espErr != ESP_OK) {
                ESP_LOGE(TAG, "i2s_write error: %s", esp_err_to_name(espErr));
                continue;
            }
            if (written != dpr.size) {
                ESP_LOGE(TAG, "is2_write() wrote less than requested with infinite timeout");
            }
        }
    }
}
void I2sSinkNode::dmaFillWithSilence()
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

bool I2sSinkNode::setFormat(StreamFormat fmt)
{
    auto bits = fmt.bits();
    if (bits != 16) {
        ESP_LOGE(TAG, "Only 16bit sample width is supported, but %d provided", bits);
        return false;
    }
    ESP_LOGI(TAG, "Setting output mode to %d-bit %s, %d Hz", bits,
        (fmt.channels() == 2) ? "stereo" : "mono", fmt.samplerate);
    auto err = i2s_set_clk(mPort, fmt.samplerate,
        (i2s_bits_per_sample_t)fmt.bits(), (i2s_channel_t)fmt.channels());
    if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "i2s_set_clk failed: rate: %d, bits: %d, ch: %d. Error: %s",
            fmt.samplerate, bits, fmt.channels(), esp_err_to_name(err));
        return false;
    }
    mFormat = fmt;
    return true;
}

I2sSinkNode::I2sSinkNode(const char* tag, int port, i2s_pin_config_t* pinCfg)
:AudioNodeWithTask(tag, kStackSize), mFormat(44100, 16, 2)
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
    cfg.sample_rate = 44100;
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
        ESP_LOGE(TAG, "Error installing i2s driver: %s", esp_err_to_name(err));
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
}

I2sSinkNode::~I2sSinkNode()
{
    i2s_driver_uninstall(mPort);
}
