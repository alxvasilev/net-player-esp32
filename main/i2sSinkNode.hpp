#ifndef I2S_SINK_NODE_HPP
#define I2S_SINK_NODE_HPP
#include <stdlib.h>
#include <string.h>
#include "audioNode.hpp"
#include <driver/i2s_std.h>
#include "volume.hpp"

class I2sOutputNode: public AudioNodeWithTask
{
public:
    struct Config {
        int8_t port;
        int8_t pin_dout;
        int8_t pin_ws;
        int8_t pin_bclk;
        int dmaBufSizeMs;
        int dmaBufSizeMax;
    };
    Mutex mutex;
    StreamId mStreamId = 0;
protected:
    enum {
        kTaskPriority = 22, kDefaultBps = 16, kDefaultSamplerate = 44100,
        kFadeInMs = 400, kFadeOutMs = 50, kTicksBeforeDacUnmute = 10
    };
    enum: uint8_t { kCommandPrefillComplete = AudioNodeWithTask::kCommandLast + 1 };
    typedef bool(I2sOutputNode::*FadeFunc)(DataPacket& pkt);
    Config mConfig;
    i2s_chan_handle_t mI2sChan = nullptr;
    StreamFormat mFormat;
    uint64_t mSampleCtr;
    FadeFunc mFadeFunc = nullptr;
    float mFadeStep = 0.0f;
    float mCurrFadeLevel = 0.0f;
    uint16_t mFadeInMs = kFadeInMs;
    uint16_t mFadeOutMs = kFadeOutMs;
    uint8_t mBytesPerSampleShiftDiv;
    uint8_t mDmaBufMillisec;
    bool mChanStarted = false;
    bool mDacMuted = false;
    PrefillEvent::IdType mLastPrefillId = 0;
    bool mWaitingPrefill = false;
    const gpio_num_t kDacMutePin = GPIO_NUM_32;
    virtual bool dispatchCommand(Command &cmd) override;
    virtual void nodeThreadFunc() override;
    virtual void onStopped() override;
    void dmaFillWithSilence();
    bool createChannel();
    bool reconfigChannel();
    bool deleteChannel();
    bool setFormat(StreamFormat fmt);
    void setDacMutePin(uint8_t level);
    void setFade(bool fadeIn);
    void muteDac();
    void unMuteDac();
    bool sendSilence();
    template <typename T, bool fadeIn>
    bool fade(DataPacket& pkt);
public:
    I2sOutputNode(IAudioPipeline& parent, Config& cfg, uint16_t stackSize, int8_t cpuCore=-1);
    ~I2sOutputNode();
    virtual Type type() const { return kTypeI2sOut; }
    virtual StreamEvent pullData(PacketResult& dpr) { return kErrStreamStopped; }
    void notifyPrefillComplete(PrefillEvent::IdType id) { mCmdQueue.post(kCommandPrefillComplete, id); }
    uint32_t positionTenthSec() const;
    void mute() { muteDac(); }
    void unmute() { unMuteDac(); }
};

#endif
