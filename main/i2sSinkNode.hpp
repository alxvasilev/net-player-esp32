#ifndef I2S_SINK_NODE_HPP
#define I2S_SINK_NODE_HPP
#include <stdlib.h>
#include <string.h>
#include "audioNode.hpp"
#include <driver/i2s.h>

class I2sOutputNode: public AudioNodeWithTask, public IAudioVolume
{
protected:
    enum: uint8_t { kVolumeDiv = 64 };
    i2s_port_t mPort;
    bool mUseInternalDac;
    StreamFormat mFormat;
    int mReadTimeout;
    uint8_t mVolume = kVolumeDiv;
    enum { kDmaBufLen = 600, kDmaBufCnt = 3,
           kStackSize = 9000, kDefaultSamplerate = 44100
    };
    virtual void nodeThreadFunc();
    template <typename T, bool ChangeVol>
    void processVolumeStereo(DataPullReq& dpr);

    template <typename T, bool ChangeVol>
    void processVolumeMono(DataPullReq& dpr);

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
    // volume interface
    virtual uint16_t getVolume() const;
    virtual void setVolume(uint16_t vol);
};

#endif
