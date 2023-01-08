#ifndef I2S_SINK_NODE_HPP
#define I2S_SINK_NODE_HPP
#include <stdlib.h>
#include <string.h>
#include "audioNode.hpp"
#include <driver/i2s.h>
#include "volume.hpp"

class I2sOutputNode: public AudioNodeWithTask, public DefaultVolumeImpl
{
public:
    Mutex mutex;
    StreamId mStreamId = 0;
protected:
    i2s_port_t mPort;
    StreamFormat mFormat;
    uint64_t mSampleCtr;
    bool mUseInternalDac;
    uint8_t mBytesPerSampleShiftDiv;
    enum {
        kTaskPriority = 20, kDefaultBps = 16, kDefaultSamplerate = 44100
    };
    virtual void nodeThreadFunc();
    void adjustSamplesForInternalDac(char* sBuff, int len);
    void dmaFillWithSilence();
    bool setFormat(StreamFormat fmt);
public:
    I2sOutputNode(IAudioPipeline& parent, int port, i2s_pin_config_t* pinCfg, uint16_t stackSize, uint8_t dmaBufCnt, int8_t cpuCore=-1);
    ~I2sOutputNode();
    virtual Type type() const { return kTypeI2sOut; }
    virtual IAudioVolume* volumeInterface() override { return this; }
    virtual StreamError pullData(DataPullReq& dpr) { return kTimeout; }
    virtual void confirmRead(int amount) {}
    uint32_t positionTenthSec() const;
};

#endif
