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
void I2sOutputNode::setDacMutePin(uint8_t level)
{
    if (kDacMutePin >= 0) {
        gpio_set_level(kDacMutePin, level);
        mDacMuted = (level == 0);
    }
}
template<typename T>
T myRound(float val)
{
    return (val < 0) ? val - 0.5f : val + 0.5f;
}

template <typename S>
bool I2sOutputNode::rampIn(void* target)
{
    typedef S Sample[2];
    enum {
        kSampleSize = sizeof(Sample),
        kDepopSampleCnt = kDepopBufSize / kSampleSize,
    };
    auto samples = (Sample*)alloca(kDepopBufSize);
    auto end = samples + kDepopSampleCnt;
    float step0 = (float)((*(Sample*)target)[0]) / kDepopSampleCnt;
    float step1 = (float)((*(Sample*)target)[1]) / kDepopSampleCnt;
    float val0 = 0; float val1 = 0;
    for (Sample* sample = samples; sample < end; sample++) {
        val0 += step0;
        val1 += step1;
        (*sample)[0] = myRound<S>(val0);
        (*sample)[1] = myRound<S>(val1);

        if (end - sample < 20) {
//          printf("%d, %d\n", (*sample)[0], (*sample)[1]);
        }

    }
//  printf("target: %d, %d\n", ((*(Sample*)target)[0]), ((*(Sample*)target)[1]));
    size_t written = 0;
    auto err = i2s_write(mPort, samples, kDepopBufSize, &written, portMAX_DELAY);
    return err == ESP_OK;
}
template <typename S>
bool I2sOutputNode::fadeIn(char* sampleData, int sampleBufSize)
{
    typedef S Sample[2];
    enum {
        kSampleSize = sizeof(Sample)
    };
    auto samples = (Sample*)sampleData;
    auto end = (Sample*)(sampleData + sampleBufSize);
    int sampleCnt = sampleBufSize / kSampleSize;
    float gainIncStep = (float)1 / sampleCnt;
    float gain = 0.0f;
    for (Sample* sample = samples; sample < end; sample++) {
        gain += gainIncStep;
        (*sample)[0] *= gain;
        (*sample)[1] *= gain;
     // printf("%d, %d\n", (*sample)[0], (*sample)[1]);
    }
    size_t written = 0;
    // printf("sending fade: %d\n", sampleBufSize);
    auto err = i2s_write(mPort, samples, kDepopBufSize, &written, portMAX_DELAY);
    return err == ESP_OK;
}

bool I2sOutputNode::sendSilence()
{
    enum { kChunkSize = 2048 };
    auto buf = (char*)alloca(kChunkSize);
    memset(buf, 0, kChunkSize);
    buf[0] = 1;
    int count = 1 + (mDmaBufCount * (1024 * mFormat.bitsPerSample() * 2 / 8) + kChunkSize - 1) / kChunkSize;
    for (int i = 0; i < count; i++) {
        size_t written;
        if (i2s_write(mPort, buf, kChunkSize, &written, portMAX_DELAY) != ESP_OK) {
            return false;
        }
    }
    return true;
}
void I2sOutputNode::nodeThreadFunc()
{
    for (;;) {
        muteDac();
        setState(kStateStopped);
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        plSendEvent(kEventAudioFormatChange, mFormat.asNumCode());
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
                if (err == kEvtStreamChanged) {
                    // dpr does not contain PCM format info, but codec type and streamId
                    MutexLocker locker(mutex);
                    mSampleCtr = 0;
                    mStreamId = dpr.streamId;
                    ESP_LOGI(mTag, "streamId set to %u", mStreamId);
                    continue;
                } else {
                    // Note: we are sending the error with dpr.streamId directly (i.e. not with mStreamId)
                    // because it may be possible that the kStreamChanged event was not have been delivered
                    // by the decoder, if it entered a confused state. This may happen with the FLAC decoder
                    plSendEvent(kEventStreamError, err, dpr.streamId);
                    // flush DMA buffers - ramp / fade out
                    vTaskDelay(20);
                    break;
                }
            }
            myassert(dpr.size);
            dpr.fmt.codec() = Codec::kCodecUnknown;
            if (dpr.fmt != mFormat) {
                ESP_LOGW(mTag, "Stream format changed");
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
            if (mDacMuted) {
                ESP_LOGI(mTag, "Sending a bit of silence...");
                sendSilence();
                vTaskDelay(1);
                unMuteDac();
#if 1
//              ESP_LOGW(mTag, "sending ramp");
                if (mFormat.bitsPerSample() <= 16) {
                    rampIn<int16_t>(dpr.buf);
                } else {
                    rampIn<int32_t>(dpr.buf);
                }
//              ESP_LOGW(mTag, "ramp sent");
#else
                if (mFormat.bitsPerSample() <= 16) {
                    fadeIn<int16_t>(dpr.buf, dpr.size);
                } else {
                    fadeIn<int32_t>(dpr.buf, dpr.size);
                }
                mPrev->confirmRead(dpr.size);
                continue;
#endif
            }
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
    plSendEvent(kEventAudioFormatChange, fmt.asNumCode());
    return true;
}

I2sOutputNode::I2sOutputNode(IAudioPipeline& parent, int port, i2s_pin_config_t* pinCfg, uint16_t stackSize,
    uint8_t dmaBufCnt, int8_t cpuCore)
:AudioNodeWithTask(parent, "node-i2s-out", stackSize, kTaskPriority, cpuCore),
  mFormat(kDefaultSamplerate, 16, 2), mDmaBufCount(dmaBufCnt)
{
    if (port == 0xff) {
        mUseInternalDac = true;
        mPort = I2S_NUM_0;
    } else {
        mUseInternalDac = false;
        mPort = (i2s_port_t)port;
    }
    if (kDacMutePin != GPIO_NUM_NC) {
        gpio_pad_select_gpio(kDacMutePin);
        gpio_set_direction(kDacMutePin, GPIO_MODE_OUTPUT);
        muteDac();
    }
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = kDefaultSamplerate;
    cfg.bits_per_sample = (i2s_bits_per_sample_t) kDefaultBps;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.dma_buf_count = dmaBufCnt;
    cfg.dma_buf_len = 1024;
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
