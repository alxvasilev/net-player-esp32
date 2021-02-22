#ifndef I2S_SINK_NODE_HPP
#define I2S_SINK_NODE_HPP
#include <stdlib.h>
#include <string.h>
#include "audioNode.hpp"
#include <driver/i2s.h>
#include "volume.hpp"

class I2sOutputNode: public AudioNodeWithTask, public DefaultVolumeImpl
{
protected:
    i2s_port_t mPort;
    bool mUseInternalDac;
    StreamFormat mFormat;
    int mReadTimeout;
    enum {
        kDmaBufLen = 1023, kDmaBufCnt = 2, // in samples, multiply by 4 for bytes
        kPipelineBufSize = 4096, // little more than the max size of one decoded frame (i.e. 4 * 1152 for mp3)
        kStackSize = 9000, kDefaultSamplerate = 44100
    };
    virtual void nodeThreadFunc();
    void adjustSamplesForInternalDac(char* sBuff, int len);
    void dmaFillWithSilence();
    bool setFormat(StreamFormat fmt);
    void recalcReadTimeout(int samplerate);
public:
    I2sOutputNode(int port, i2s_pin_config_t* pinCfg);
    ~I2sOutputNode();
    virtual Type type() const { return kTypeI2sOut; }
    virtual IAudioVolume* volumeInterface() override { return this; }
    virtual StreamError pullData(DataPullReq& dpr, int timeout) { return kTimeout; }
    virtual void confirmRead(int amount) {}
};

#endif
