#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.hpp"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "i2sSinkNode.hpp"
#include <type_traits>
#include <limits>
#include <math.h> // for roundf
#define DEBUG_TIMING 1

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
    auto err = i2s_channel_write(mI2sChan, samples, kDepopBufSize, nullptr, 0xffffffff);
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
    auto err = ESP_OK; //i2s_write(mPinConfig.port, samples, kDepopBufSize, &written, portMAX_DELAY);
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
        if (i2s_channel_write(mI2sChan, buf, kChunkSize, nullptr, 0xffffffff) != ESP_OK) {
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
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            PacketResult dpr;
#ifdef DEBUG_TIMING
            ElapsedTimer t;
#endif
            auto evt = mPrev->pullData(dpr);
#ifdef DEBUG_TIMING
            auto elapsed = t.msElapsed();
            if (elapsed >= 40) {
                ESP_LOGI(mTag, "pullData took %d ms", elapsed);
            }
#endif
            if (evt) {
                if (evt < 0) {
                    plSendError(evt, dpr.streamId);
                    // flush DMA buffers - ramp / fade out
                    vTaskDelay(20);
                    break;
                }
                else if (evt == kEvtTitleChanged) {
                    plSendEvent(kEventTrackInfo, 0, (uintptr_t)(*(TitleChangeEvent*)dpr.packet.get()).title);
                }
                else { // any other generic event
                    if (evt == kEvtStreamChanged) {
                        auto& pkt = dpr.newStreamEvent();
                        ESP_LOGI(mTag, "Got start of new stream with streamId %u", pkt.streamId);
                        MutexLocker locker(mutex);
                        setFormat(pkt.fmt);
                        mSampleCtr = 0;
                        mStreamId = pkt.streamId;
                        plSendEvent(kEventNewStream, 0, (uintptr_t)dpr.packet.get());
                        plSendEvent(kEventPlaying);
                    }
                    else if (evt == kEvtStreamEnd) {
                        plSendEvent(kEventStreamEnd, 0, dpr.genericEvent().streamId);
                    }
                }
                continue;
            }
            // we have data
            auto& pkt = dpr.dataPacket();
            myassert(pkt.dataLen);
            {
                MutexLocker locker(mutex);
                mSampleCtr += pkt.dataLen >> mBytesPerSampleShiftDiv;
            }
            if (mDacMuted) {
                ESP_LOGI(mTag, "Sending a bit of silence...");
//                sendSilence();
                vTaskDelay(1);
                unMuteDac();
#if 0
#if 1
//              ESP_LOGW(mTag, "sending ramp");
                if (mFormat.bitsPerSample() <= 16) {
                    rampIn<int16_t>(pkt.data);
                } else {
                    rampIn<int32_t>(pkt.data);
                }
//              ESP_LOGW(mTag, "ramp sent");
#else
                if (mFormat.bitsPerSample() <= 16) {
                    fadeIn<int16_t>(pkt.data, pkt.dataLen);
                } else {
                    fadeIn<int32_t>(pkt.data, pkt.dataLen);
                }
                //TODO: Revise DAC muting
                continue;
#endif
#endif
            }
            size_t written;
            MY_ESP_ERRCHECK(i2s_channel_write(mI2sChan, pkt.data, pkt.dataLen, &written, 0xffffffff), mTag,
                "calling i2s_channel_write()", continue);
            if (written != pkt.dataLen) {
                ESP_LOGW(mTag, "is2_channel_write() wrote less than requested with infinite timeout");
            }
        }
    }
}

bool I2sOutputNode::setFormat(StreamFormat fmt)
{
    if (fmt == mFormat) {
        return true;
    }
    auto newBps = fmt.bitsPerSample();
/*
    if (bps == 24) {
        samplerate -= roundf(samplerate * 27.0f / 440);
    }
*/
    ESP_LOGW(mTag, "Setting output mode to %u-bit %s, %lu Hz", newBps, fmt.isStereo() ? "stereo" : "mono",
             fmt.sampleRate());
    deleteChannel();
    mFormat = fmt;
    return createChannel();
/*
    auto oldFmt = mFormat;
    mFormat = fmt;
    if ((newBps <= 16) != (oldFmt.bitsPerSample() <= 16)) {
        ESP_LOGW(mTag, "Sample width changed, re-creating i2s channel");
        deleteChannel();
        return createChannel();
    }
    else {
        return reconfigChannel();
    }
*/
}
bool I2sOutputNode::reconfigChannel() {
    assert(mI2sChan);
    auto bps = mFormat.bitsPerSample();
    MY_ESP_ERRCHECK(i2s_channel_disable(mI2sChan), mTag, "disabling i2s channel", return false);
    i2s_std_clk_config_t clockCfg = I2S_STD_CLK_DEFAULT_CONFIG(mFormat.sampleRate()); /* {
        .sample_rate_hz = samplerate,
        .clk_src = I2S_CLK_SRC_APLL,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384 // must be multiple of 3
    }; */
    MY_ESP_ERRCHECK(i2s_channel_reconfig_std_clock(mI2sChan, &clockCfg), mTag, "setting I2S clock", return false);

    i2s_std_slot_config_t slotCfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
        (i2s_data_bit_width_t)bps,
        (mFormat.numChannels() > 1 ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO)
    );
    MY_ESP_ERRCHECK(i2s_channel_reconfig_std_slot(mI2sChan, &slotCfg), mTag, "setting sample format", return false);

    uint bytesPerSample = mFormat.numChannels() * (bps / 8);
    for (mBytesPerSampleShiftDiv = 0; bytesPerSample; bytesPerSample >>= 1, mBytesPerSampleShiftDiv++);
    MY_ESP_ERRCHECK(i2s_channel_enable(mI2sChan), mTag, "enabling channel",);
    return true;
}
I2sOutputNode::I2sOutputNode(IAudioPipeline& parent, PinCfg& pins, uint16_t stackSize,
    uint8_t dmaBufCnt, int8_t cpuCore)
:AudioNodeWithTask(parent, "node-i2s-out", false, stackSize, kTaskPriority, cpuCore),
  mPinConfig(pins), mFormat(kDefaultSamplerate, 16, 2), mDmaBufCount(dmaBufCnt)
{
    if (kDacMutePin != GPIO_NUM_NC) {
        esp_rom_gpio_pad_select_gpio(kDacMutePin);
        gpio_set_direction(kDacMutePin, GPIO_MODE_OUTPUT);
        muteDac();
    }
    createChannel();
}
bool I2sOutputNode::createChannel()
{
    assert(!mI2sChan);
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cfg.dma_desc_num = mDmaBufCount;
    cfg.dma_frame_num = mFormat.bitsPerSample() <= 16 ? 1023 : 510;
    cfg.intr_priority = 3;
    MY_ESP_ERRCHECK(i2s_new_channel(&cfg, &mI2sChan, NULL), mTag, "creating i2s channel",
        mI2sChan = nullptr;
        return false;
    );

    i2s_std_config_t cfgInit = {
        .clk_cfg = {
            .sample_rate_hz = mFormat.sampleRate(),
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = (mFormat.bitsPerSample() == 24) ? I2S_MCLK_MULTIPLE_192 : I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG((i2s_data_bit_width_t)mFormat.bitsPerSample(), I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // some codecs may require mclk signal
            .bclk = (gpio_num_t)mPinConfig.bclk,
            .ws   = (gpio_num_t)mPinConfig.ws,
            .dout = (gpio_num_t)mPinConfig.dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    MY_ESP_ERRCHECK(i2s_channel_init_std_mode(mI2sChan, &cfgInit), mTag, "configuring I2S channel",
        i2s_del_channel(mI2sChan);
        mI2sChan = nullptr;
        return false;
    );
    i2s_channel_enable(mI2sChan);
    return true;
}

bool I2sOutputNode::deleteChannel()
{
    if (!mI2sChan) {
        return true;
    }
    bool ok = true;
    MY_ESP_ERRCHECK(i2s_channel_disable(mI2sChan), mTag, "disabling i2s channel", ok = false);
    MY_ESP_ERRCHECK(i2s_del_channel(mI2sChan), mTag, "deleting i2s channel", ok = false);
    mI2sChan = nullptr;
    return ok;
}
I2sOutputNode::~I2sOutputNode()
{
    terminate(true);
    deleteChannel();
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
