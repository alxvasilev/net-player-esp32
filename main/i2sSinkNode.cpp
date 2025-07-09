#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "utils.hpp"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "i2sSinkNode.hpp"
#include <magic_enum.hpp>
#include <limits>
#define DEBUG_TIMING 1

void I2sOutputNode::setDacMutePin(uint8_t level)
{
    if (kDacMutePin >= 0) {
        gpio_set_level(kDacMutePin, level);
        mDacMuted = (level == 0);
    }
}
void I2sOutputNode::muteDac()
{
    setDacMutePin((gpio_num_t)0);
    ESP_LOGI(mTag, "DAC muted");
}
void I2sOutputNode::unMuteDac()
{
    setDacMutePin((gpio_num_t)1);
    ESP_LOGI(mTag, "DAC unmuted");
}
void I2sOutputNode::setFade(bool fadeIn)
{
    if (!mFormat) {
        ESP_LOGE(mTag, "setFade: Output format not initialized");
        return;
    }
    float step = 1.0f / ((float)mFormat.sampleRate() * (fadeIn ? mFadeInMs : mFadeOutMs) / 1000);
    auto bps = mFormat.bitsPerSample();
    if (fadeIn) {
        mFadeStep = step;
        mCurrFadeLevel = 0.0f;
        mFadeFunc = bps <= 16 ? &I2sOutputNode::fade<int16_t, true> : &I2sOutputNode::fade<int32_t, true>;
    }
    else {
        mFadeStep = -step;
        mCurrFadeLevel = 1.0f;
        mFadeFunc = bps <= 16 ? &I2sOutputNode::fade<int16_t, false> : &I2sOutputNode::fade<int32_t, false>;
    }
}
template <typename T, bool fadeIn>
bool I2sOutputNode::fade(DataPacket& pkt)
{
    myassert(fadeIn == (mFadeStep > 0));
    auto sample = (T*)pkt.data;
    auto end = (T*)(pkt.data + pkt.dataLen);
    while(sample < end) {
        *sample = (float)*sample * mCurrFadeLevel + .5f;
        sample++;
        *sample = (float)*sample * mCurrFadeLevel + .5f;
        sample++;
        mCurrFadeLevel += mFadeStep;
        if (fadeIn) {
            if (mCurrFadeLevel >= 1.0f) {
                mFadeFunc = nullptr;
                return false;
            }
        }
        else {
            if (mCurrFadeLevel < 0.0f) {
                mFadeFunc = nullptr;
                return false;
            }
        }
    }
    return true;
}
bool I2sOutputNode::dispatchCommand(Command &cmd)
{
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    if (cmd.opcode == kCommandPrefillComplete) {
        auto id = (PrefillEvent::IdType)cmd.arg;
        if (mWaitingPrefill && (id >= mLastPrefillId)) {
            mWaitingPrefill = 0;
            mLastPrefillId = id;
            ESP_LOGI(mTag, "Got prefill %d complete command, starting playback", id);
            plSendEvent(kEventPlaying);
            setState(kStateRunning);
        }
        else {
            ESP_LOGW(mTag, "Ignoring PrefillComplete (id=%d) command that doesn't match our state (prefillId=%d, waitingPrefill=%d)", id, mLastPrefillId, mWaitingPrefill);
        }
        return true;
    }
    return false;
}
void I2sOutputNode::onStopped()
{
    muteDac();
}
void I2sOutputNode::nodeThreadFunc()
{
    muteDac();
    for (;;) {
        setState(kStateStopped);
        processMessages();
        if (mTerminate) {
            return;
        }
        if (mWaitingPrefill) {
            ESP_LOGI(mTag, "Prefill wait: stopping and waiting for command...");
            continue;
        }
        myassert(mState == kStateRunning);
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            PacketResult dpr;
#ifdef DEBUG_TIMING
            ElapsedTimer t;
#endif
            auto evt = mPrev->pullData(dpr);
#ifdef DEBUG_TIMING
            auto elapsed = t.usElapsed();
#if DEBUG_TIMING < 2
            if (elapsed >= 12000) {
#endif
                ESP_LOGI(mTag, "pullData took %d ms", (int)((elapsed + 500) / 1000));
#if DEBUG_TIMING < 2
            }
#endif
#endif
            if (evt) {
                if (evt < 0) {
                    ESP_LOGI(mTag, "Got stream error %s", magic_enum::enum_name(evt).data());
                    plSendError(evt, dpr.streamId);
                    // flush DMA buffers - ramp / fade out
                    vTaskDelay(20);
                    break;
                }
                else if (evt == kEvtTitleChanged) {
                    auto& titleEvent = dpr.titleEvent();
                    plSendEvent(kEventTitleChanged,
                        (uintptr_t)titleEvent.title.release(),
                        (uintptr_t)titleEvent.artist.release());
                }
                else { // any other generic event
                    if (evt == kEvtStreamChanged) {
                        auto& pkt = dpr.newStreamEvent();
                        ESP_LOGI(mTag, "Got start of new stream with streamId %u", pkt.streamId);
                        MutexLocker locker(mutex);
                        if (pkt.fmt != mFormat) {
                            if (!setFormat(pkt.fmt)) {
                                plSendError(kErrStreamFmt, 0);
                                break;
                            }
                        }
                        else {
                            setFade(true);
                        }
                        mSampleCtr = 0;
                        mStreamId = pkt.streamId;
                        plSendEvent(kEventNewStream, 0, (uintptr_t)dpr.packet.get());
                    }
                    else if (evt == kEvtStreamEnd) {
                        plSendEvent(kEventStreamEnd, dpr.genericEvent().streamId);
                    }
                    else if (evt == kEvtPrefill) {
                        auto& prefillEvent = dpr.prefillEvent();
                        auto diff = prefillEvent.prefillId - mLastPrefillId;
                        // take prefillId wrap into account
                        if (diff > 0) {
                            mLastPrefillId = prefillEvent.prefillId;
                            mWaitingPrefill = true;
                            plSendEvent(kEventBuffering);
                            ESP_LOGI(mTag, "Got wait-prefill event for prefill %d, halting till uncorked...",
                                prefillEvent.prefillId);
                            break;
                        }
                    }
                }
                continue;
            }
            if (!mStreamId) { // we haven't had a proper stream start with audio format info
                continue;
            }
            // we have data
            auto& pkt = dpr.dataPacket();
            myassert(pkt.dataLen);
            {
                MutexLocker locker(mutex);
                mSampleCtr += pkt.dataLen >> mBytesPerSampleShiftDiv;
            }
            if (mFadeFunc) {
                (this->*mFadeFunc)(pkt);
            }
            size_t written = 0;
            if (!mChanStarted) {
                MY_ESP_ERRCHECK(i2s_channel_preload_data(mI2sChan, pkt.data, pkt.dataLen, &written), mTag, "pre-loading DMA", continue);
                if (written == pkt.dataLen) {
                    continue;
                }
                i2s_channel_enable(mI2sChan);
                mChanStarted = true;
                plSendEvent(kEventPlaying);
                vTaskDelay(kTicksBeforeDacUnmute);
                unMuteDac();
                MY_ESP_ERRCHECK(i2s_channel_write(mI2sChan, pkt.data + written, pkt.dataLen - written,
                    &written, 0xffffffff), mTag, "calling i2s_channel_write()", continue);
            }
            else {
                MY_ESP_ERRCHECK(i2s_channel_write(mI2sChan, pkt.data, pkt.dataLen, &written, 0xffffffff), mTag,
                    "calling i2s_channel_write()", continue);
                if (written != pkt.dataLen) {
                    ESP_LOGW(mTag, "is2_channel_write() wrote less than requested, with infinite timeout");
                }
                if (mDacMuted) {
                    vTaskDelay(kTicksBeforeDacUnmute);
                    unMuteDac();
                }
            }
        }
    }
}

bool I2sOutputNode::setFormat(StreamFormat fmt)
{
    auto newBps = fmt.bitsPerSample();
    ESP_LOGW(mTag, "Setting output mode to %u-bit %s, %lu Hz", newBps, fmt.isStereo() ? "stereo" : "mono",
             fmt.sampleRate());
    deleteChannel();
    mFormat = fmt;
    setFade(true);
    return createChannel();
}
bool I2sOutputNode::reconfigChannel()
{
    assert(mI2sChan);
    auto bps = mFormat.bitsPerSample();
    MY_ESP_ERRCHECK(i2s_channel_disable(mI2sChan), mTag, "disabling i2s channel", return false);
    i2s_std_clk_config_t clockCfg = {
        .sample_rate_hz = mFormat.sampleRate(),
        .clk_src = I2S_CLK_SRC_APLL,
        .mclk_multiple = (mFormat.bitsPerSample() == 24) ? I2S_MCLK_MULTIPLE_192 : I2S_MCLK_MULTIPLE_256
    };
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
I2sOutputNode::I2sOutputNode(IAudioPipeline& parent, Config& cfg, uint16_t stackSize, int8_t cpuCore)
:AudioNodeWithTask(parent, "node-i2s-out", true, stackSize, kTaskPriority, cpuCore),
  mConfig(cfg), mFormat(kDefaultSamplerate, 16, 2)
{
    if (kDacMutePin != GPIO_NUM_NC) {
        esp_rom_gpio_pad_select_gpio(kDacMutePin);
        gpio_set_direction(kDacMutePin, GPIO_MODE_OUTPUT);
        muteDac();
    }
}
bool I2sOutputNode::createChannel()
{
    assert(!mI2sChan);
    assert(!mChanStarted);
    auto bps = mFormat.bitsPerSample();
    auto sr = mFormat.sampleRate();
    auto nChans = mFormat.numChannels();
    int millis = mConfig.dmaBufSizeMs;
    int sampleSize = (bps <= 16 ? 2 : 4) * nChans;
    int dmaSize = (sampleSize * sr * millis + 999) / 1000;
    if (dmaSize > mConfig.dmaBufSizeMax) {
        dmaSize = mConfig.dmaBufSizeMax;
        int byteRate = sr * sampleSize;
        millis = (1000 * dmaSize + byteRate / 2) / byteRate;
    }
    int dmaNbufs = (dmaSize + 4091) / 4092;
    int dmaBufSamples = (dmaSize / dmaNbufs) / sampleSize;
    ESP_LOGW(mTag, "Allocating %d ms of DMA buffer: %d bytes (%d units x %d bytes(%d samples))",
        millis, dmaSize, dmaNbufs, dmaBufSamples * sampleSize, dmaBufSamples);
    i2s_chan_config_t cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cfg.dma_desc_num = dmaNbufs;
    cfg.dma_frame_num = dmaBufSamples;
    cfg.intr_priority = 3;
    cfg.auto_clear = true;
    MY_ESP_ERRCHECK(i2s_new_channel(&cfg, &mI2sChan, NULL), mTag, "creating i2s channel",
        mI2sChan = nullptr;
        return false;
    );

    i2s_std_config_t cfgInit = {
        .clk_cfg = {
            .sample_rate_hz = sr,
            .clk_src = I2S_CLK_SRC_APLL,
            .mclk_multiple = (bps == 24) ? I2S_MCLK_MULTIPLE_192 : I2S_MCLK_MULTIPLE_256
        },
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(
            (i2s_data_bit_width_t)mFormat.bitsPerSample(),
            ((nChans == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO)),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // some codecs may require mclk signal
            .bclk = (gpio_num_t)mConfig.pin_bclk,
            .ws   = (gpio_num_t)mConfig.pin_ws,
            .dout = (gpio_num_t)mConfig.pin_dout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false }
        }
    };
    MY_ESP_ERRCHECK(i2s_channel_init_std_mode(mI2sChan, &cfgInit), mTag, "configuring I2S channel",
        i2s_del_channel(mI2sChan);
        mI2sChan = nullptr;
        return false;
    );
    return true;
}

bool I2sOutputNode::deleteChannel()
{
    if (!mI2sChan) {
        return true;
    }
    bool ok = true;
    MY_ESP_ERRCHECK(i2s_channel_disable(mI2sChan), mTag, "disabling i2s channel", ok = false);
    mChanStarted = false;
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
