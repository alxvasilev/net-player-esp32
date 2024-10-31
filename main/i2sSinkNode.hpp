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
    PinCfg mPinConfig;
    i2s_chan_handle_t mI2sChan = nullptr;
    StreamFormat mFormat;
    uint64_t mSampleCtr;
    uint8_t mBytesPerSampleShiftDiv;
    uint8_t mDmaBufCount; // needed for flushing with silence
    bool mDacMuted = false;
    enum {
        kTaskPriority = 20, kDefaultBps = 16, kDefaultSamplerate = 44100, kDepopBufSize = 2048
    };
    const gpio_num_t kDacMutePin = GPIO_NUM_32;
    virtual void nodeThreadFunc();
    void adjustSamplesForInternalDac(char* sBuff, int len);
    void dmaFillWithSilence();
    bool createChannel();
    bool reconfigChannel();
    bool deleteChannel();
    bool setFormat(StreamFormat fmt);
    void setDacMutePin(uint8_t level);
    void muteDac() { setDacMutePin((gpio_num_t)0); ESP_LOGI(mTag, "DAC muted"); }
    void unMuteDac() { setDacMutePin((gpio_num_t)1);  ESP_LOGI(mTag, "DAC unmuted");}
    bool sendSilence();
    template <typename S>
    bool rampIn(void* targetSample);
    template <typename S>
    bool fadeIn(char* sampleBuf, int sampleBufSize);
public:
    I2sOutputNode(IAudioPipeline& parent, PinCfg& pins, uint16_t stackSize,
        uint8_t dmaBufCnt, int8_t cpuCore=-1);
    ~I2sOutputNode();
    virtual Type type() const { return kTypeI2sOut; }
    virtual StreamEvent pullData(PacketResult& dpr) { return kErrStreamStopped; }
    uint32_t positionTenthSec() const;
    void mute() { muteDac(); }
    void unmute() { unMuteDac(); }
};

#endif
