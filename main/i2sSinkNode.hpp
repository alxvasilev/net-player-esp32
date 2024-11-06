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
    struct PinCfg {
        int8_t port;
        int8_t dout;
        int8_t ws;
        int8_t bclk;
    };
    Mutex mutex;
    StreamId mStreamId = 0;
protected:
    enum {
        kTaskPriority = 20, kDefaultBps = 16, kDefaultSamplerate = 44100, kDepopBufSize = 2048,
        kFadeInMs = 400, kFadeOutMs = 50, kTicksBeforeDacUnmute = 10, kDmaBufSizeMax = 40000
    };
    typedef bool(I2sOutputNode::*FadeFunc)(DataPacket& pkt);
    PinCfg mPinConfig;
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
    const gpio_num_t kDacMutePin = GPIO_NUM_32;
    virtual void nodeThreadFunc();
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
    I2sOutputNode(IAudioPipeline& parent, PinCfg& pins, uint16_t stackSize,
        uint8_t dmaMillis, int8_t cpuCore=-1);
    ~I2sOutputNode();
    virtual Type type() const { return kTypeI2sOut; }
    virtual StreamEvent pullData(PacketResult& dpr) { return kErrStreamStopped; }
    uint32_t positionTenthSec() const;
    void mute() { muteDac(); }
    void unmute() { unMuteDac(); }
};

#endif
