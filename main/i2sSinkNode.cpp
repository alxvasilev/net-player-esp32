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
#include <type_traits>
#include <limits>
#include <math.h> // for roundf

void I2sOutputNode::adjustSamplesForInternalDac(char* sBuff, int len)
{
    int16_t* buf16 = (int16_t*)sBuff;
    auto end = buf16 + len / 2;
    for(; buf16 < end; buf16++) {
        (*buf16) &= 0xff00;
        (*buf16) += 0x8000;//turn signed value into unsigned, expand negative value into positive range
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
        plSendEvent(kEventAudioFormatChange, 0, mFormat.asCode());
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            DataPullReq dpr(0xffff); // read all available data
#ifdef DEBUG_TIMING
            ElapsedTimer t;
#endif
            auto err = mPrev->pullData(dpr);
#ifdef DEBUG_TIMING
            ESP_LOGI(TAG, "pullData took %d ms\n", t.msElapsed());
#endif
            if (err) {
                if (err == kStreamChanged) {
                    // dpr does not contain PCM format info, but codec type and streamId
                    MutexLocker locker(mutex);
                    mSampleCtr = 0;
                    mStreamId = dpr.streamId;
                    ESP_LOGI(mTag, "streamId set to %u", mStreamId);
                    continue;
                } else {
                    plSendEvent(kEventStreamError, mStreamId, err);
                    break;
                }
            }
            myassert(dpr.size);
            if (dpr.fmt != mFormat) {
                ESP_LOGI(mTag, "Changing I2S output format");
                setFormat(dpr.fmt);
            }
            {
                MutexLocker locker(mutex);
                mSampleCtr += dpr.size >> mBytesPerSampleShiftDiv;
            }
            if (volProcessingEnabled()) { // enabled only when there is no equalizer node
                volumeProcess(dpr);
            }
            if (volLevelEnabled()) {
                // notify previous levels before finding new ones, thus the delay compensates a bit
                // for the delay introduced by the DMA buffering
                volumeNotifyLevelCallback();
                volumeGetLevel(dpr);
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

bool I2sOutputNode::setFormat(StreamFormat fmt)
{
    uint32_t bps = fmt.bitsPerSample();
    auto samplerate = fmt.sampleRate();
    if (bps == 24) {
        samplerate -= roundf(samplerate * 27.0f / 440);
    }

    ESP_LOGW(mTag, "Setting output mode to %d-bit %s, %d Hz", bps,
        fmt.isStereo() ? "stereo" : "mono", samplerate);
    auto err = i2s_set_clk(mPort, samplerate,
        bps, (i2s_channel_t)fmt.numChannels());
    if (err == ESP_FAIL) {
        ESP_LOGE(mTag, "i2s_set_clk failed: rate: %d, bits: %d, ch: %d. Error: %s",
            samplerate, bps, fmt.numChannels(), esp_err_to_name(err));
        return false;
    }
    mFormat = fmt;
    uint bytesPerSample = fmt.numChannels() * (bps / 8);
    for (mBytesPerSampleShiftDiv = 0; bytesPerSample; bytesPerSample >>= 1, mBytesPerSampleShiftDiv++);
    plSendEvent(kEventAudioFormatChange, 0, fmt.asCode());
    return true;
}

I2sOutputNode::I2sOutputNode(IAudioPipeline& parent, int port, i2s_pin_config_t* pinCfg, uint32_t stackSize, int8_t cpuCore)
:AudioNodeWithTask(parent, "node-i2s-out", stackSize, 20, cpuCore), mFormat(kDefaultSamplerate, 16, 2)
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
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = utils::haveSpiRam() ? kDmaBufCntSpiRam : kDmaBufCntInternalRam;
    cfg.dma_buf_len = kDmaBufLen;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL3|ESP_INTR_FLAG_IRAM;
    cfg.tx_desc_auto_clear = true;
    cfg.use_apll = true;

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
    i2s_zero_dma_buffer(mPort);
}

I2sOutputNode::~I2sOutputNode()
{
    i2s_driver_uninstall(mPort);
}

uint32_t I2sOutputNode::positionTenthSec() const
{
    auto sr = mFormat.sampleRate();
    uint32_t divider = (sr / 10);
    if (divider == 0) {
        return 0;
    }
    return (mSampleCtr + (divider >> 1)) / divider;
}
