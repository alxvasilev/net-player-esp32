#ifndef I2S_SINK_NODE_HPP
#define I2S_SINK_NODE_HPP
#include <stdlib.h>
#include <string.h>
#include "audioNode.hpp"

class I2sSinkNode: public AudioNodeWithTask
{
protected:
    i2s_port_t mPort;
    bool mUseInternalDac;
    StreamFormat mFormat;
    enum { kDmaBufLen = 600, kDmaBufCnt = 3, kStackSize = 9000 };
    virtual void nodeThreadFunc();
    void adjustSamplesForInternalDac(char* sBuff, int len);
    void dmaFillWithSilence();
    bool setFormat(StreamFormat fmt);
    virtual void doStop() { mTerminate = true; }
public:
    I2sSinkNode(const char* tag, int port, i2s_pin_config_t* pinCfg);
    ~I2sSinkNode();
    virtual StreamError pullData(DataPullReq& dpr, int timeout) { return kTimeout; }
    virtual void confirmRead(int amount) {}
};

#endif
