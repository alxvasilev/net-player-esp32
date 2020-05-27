#ifndef __ST7735_H__
#define __ST7735_H__

#include <string.h>
#include "font5x7.h"
#include "font7x11.h"
#include "color565.h"
#include <stdint.h>
#include <driver/spi_master.h>
#include <vector>

class ST7735Display
{
public:
    struct PinCfg
    {
        spi_host_device_t spiHost;
        uint8_t clk;
        uint8_t mosi;
        uint8_t cs;
        uint8_t dc; // data/command
        uint8_t rst;
    };
    typedef int16_t coord_t;
    enum Orientation
    {
      kOrientNormal = 0,
      kOrientCW     = 1,
      kOrientCCW    = 2,
      kOrient180    = 3
    };
protected:
    int16_t mWidth;
    int16_t mHeight;
    spi_device_handle_t mSpi;
    uint8_t mRstPin;
    uint8_t mDcPin;
    static void preTransferCallback(spi_transaction_t *t);
    void setRstLevel(int level);
    void displayReset();
public:
    static void usDelay(uint32_t us);
    static void msDelay(uint32_t ms);
    static uint16_t mkcolor(uint8_t R, uint8_t G, uint8_t B);

    void init(int16_t width, int16_t height, PinCfg& pins);
    void sendCmd(uint8_t opcode);
    void sendData(const uint8_t* data, int len);
    void sendData(const std::vector<uint8_t>& data);
    void setOrientation(Orientation orientation);
    void setWriteWindow(uint16_t XS, uint16_t YS, uint16_t XE, uint16_t YE);
    void fillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
    void clear(uint16_t color);
    void setPixel(uint16_t x, uint16_t y, uint16_t color);
    void hLine(uint16_t x1, uint16_t x2, uint16_t y, uint16_t color);
    void vLine(uint16_t x, uint16_t y1, uint16_t y2, uint16_t color);
    void rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
    void line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);
};

// Some ready-made 16-bit ('565') color settings:
#define ST77XX_BLACK 0x0000
#define ST77XX_WHITE 0xFFFF
#define ST77XX_RED 0xF800
#define ST77XX_GREEN 0x07E0
#define ST77XX_BLUE 0x001F
#define ST77XX_CYAN 0x07FF
#define ST77XX_MAGENTA 0xF81F
#define ST77XX_YELLOW 0xFFE0
#define ST77XX_ORANGE 0xFC00

#endif
