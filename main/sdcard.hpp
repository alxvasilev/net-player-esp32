#ifndef SDCARD_HPP
#define SDCARD_HPP

#include <stdint.h>

class SDCard
{
public:
    struct PinCfg
    {
        uint8_t clk;
        uint8_t mosi;
        uint8_t miso;
        uint8_t cs;
    };
    SDCard(){}
    bool init(int port, const PinCfg pins, const char* mountPoint = "/sdcard");
    void unmount();
    bool test();
};
#endif
