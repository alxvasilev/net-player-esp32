#ifndef SPI_HPP
#define SPI_HPP
#include <stdint.h>
#include <soc/spi_struct.h>
#include <initializer_list>
#include <soc/spi_reg.h>
#include <esp_log.h>
struct SpiPinCfg
{
    uint8_t clk;
    uint8_t mosi;
    uint8_t cs;
};

class SpiMaster
{
protected:
    uint8_t mSpiHost;
    volatile spi_dev_t& mReg;
    volatile uint32_t* fifo() const
    { return (volatile uint32_t*)((uint8_t*)(&mReg) + 0x80); }
    void configPins(const SpiPinCfg& pins);
public:
    SpiMaster(uint8_t spiHost);
    void init(const SpiPinCfg& pins, int freqDiv);
    template <bool Wait=true, typename T>
    void spiSendVal(T val)
    {
        static_assert(sizeof(val) <= sizeof(uint32_t), "");
        if (Wait) {
            waitDone();
        }
        *fifo() = val;
        fifoSend(sizeof(val));
    }
    void spiSend(const void* data, int len);
    void spiSend(const std::initializer_list<uint8_t>& data) {
        spiSend(data.begin(), (int)data.size());
    }
    void startTransaction() { mReg.cmd.usr = 1; }
    void waitDone() const { while(mReg.cmd.usr); }
    void fifoMemset(uint32_t val, int nWords=16);
    void fifoSend(int len) {
        waitDone();
        mReg.mosi_dlen.usr_mosi_dbitlen = (len << 3) - 1;
        startTransaction();
    }
};
#endif
