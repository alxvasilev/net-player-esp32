#ifndef SPDIF_HPP_INCLIDED
#define SPDIF_HPP_INCLUDED

#include "audioPlayer.hpp"
#include "driver/rmt.h"
class RmtRxChannel
{
public:
    enum { kFifoSize = 1024, kFifoClearSize = 256 };
    typedef volatile decltype(RMT.conf_ch[0].conf0) Conf0Reg;
    typedef volatile decltype(RMT.conf_ch[0].conf1) Conf1Reg;
protected:
     Conf0Reg& mConf0Reg;
     Conf1Reg& mConf1Reg;
     uint8_t mChanNo;
     void clearInterrupts();
public:
     RmtRxChannel(uint8_t chanNo):
         mConf0Reg(RMT.conf_ch[chanNo].conf0),
         mConf1Reg(RMT.conf_ch[chanNo].conf1),
         mChanNo(chanNo)
     {}
     void init(gpio_num_t pin, bool initPinGpio);
     uint8_t chanNo() const { return mChanNo; }
     void enable() { mConf1Reg.rx_en = true; }
     void stop() { mConf1Reg.rx_en = false; }
     void start() {
         resetRxPointer();
         setMemOwner(true);
         enable();
     }
     void zeroFifo() {
         //memset((uint8_t*)memory() + (kFifoSize - kFifoClearSize), 0, kFifoClearSize);
         memset((uint8_t*)memory(), 0, kFifoClearSize);
     }
     void resetRxPointer()
     {
         mConf1Reg.mem_wr_rst = 1;
         mConf1Reg.mem_wr_rst = 0;
     }
     void setMemOwner(bool isHardware) {
         mConf1Reg.mem_owner = isHardware;
     }
     volatile rmt_item32_t* memory() const {
         return RMTMEM.chan[mChanNo].data32;
     }
     static uint32_t memSize() { return 1024; }
     uint32_t status() { return RMT.status_ch[mChanNo]; }
};

class RmtRx
{
public:
    enum { kMaxPulsesPerBatch = 480 };
protected:
    RmtRxChannel mChan0;
    int8_t mActiveChanNo = -1;
    bool mIsFirstBatch = true;
    gpio_num_t mInputPin;
    RmtRxChannel mChan1;
    uint16_t mPulseClockDuration = 0;
    int16_t pcntCurrPulseCount();
    void pcntReset();
    void pcntInit();
public:
    RmtRx(gpio_num_t inputPin);
    int8_t activeChanNo() const { return mActiveChanNo; }
    void start();
    volatile rmt_item32_t* getPulses();
};

class SpdifInputNode: public AudioNodeWithTask
{
protected:
    RmtRx mRmtRx;
    void parseSpdifPulses(rmt_item32_t* data, size_t count);
public:
    SpdifInputNode(gpio_num_t inputPin);
    virtual Type type() const { return kTypeSpdifIn; }
    virtual void nodeThreadFunc() override;
    virtual StreamError pullData(DataPullReq &dpr, int timeout) override
    {
        vTaskDelay(portMAX_DELAY);
        return AudioNode::kNoError;
    }
    virtual void confirmRead(int size) override { }
};

#endif
