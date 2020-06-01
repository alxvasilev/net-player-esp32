#ifndef __ST7735_H__
#define __ST7735_H__

#include <string.h>
#include <stdint.h>
#include <driver/spi_master.h>
#include <endian.h>
#include <initializer_list>
#include "stdfonts.hpp"

class Font;

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
    spi_transaction_t mTrans;
    uint16_t mBgColor = 0x0000;
    uint16_t mFgColor = 0xffff;
    const Font* mFont = &Font_5x7;
    uint8_t mFontScale = 1;
    static void preTransferCallback(spi_transaction_t *t);
    void setRstLevel(int level);
    void displayReset();
    void setPixelRaw(uint16_t x, uint16_t y, uint16_t color);
    void fillRectRaw(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
public:
    int16_t cursorX = 0;
    int16_t cursorY = 0;
    const Font* font() const { return mFont; }
    static void usDelay(uint32_t us);
    static void msDelay(uint32_t ms);
    static uint16_t rgb(uint8_t R, uint8_t G, uint8_t B);
    ST7735Display();
    void setFgColor(uint16_t color) { mFgColor = htobe16(color); }
    void setBgColor(uint16_t color) { mBgColor = htobe16(color); }
    void gotoXY(int16_t x, int16_t y) { cursorX = x; cursorY = y; }
    void init(int16_t width, int16_t height, const PinCfg& pins);
    void sendCmd(uint8_t opcode);
    void sendCmd(uint8_t opcode, const std::initializer_list<uint8_t>& data);
    void sendData(const void* data, int len);
    void sendData(const std::initializer_list<uint8_t>& data) {
        sendData(data.begin(), (int)data.size());
    }
    void prepareSendPixels();
    void sendNextPixel(uint16_t pixel);
    void setOrientation(Orientation orientation);
    void setWriteWindow(uint16_t XS, uint16_t YS, uint16_t XE, uint16_t YE);
    void fillRect(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
        fillRectRaw(x0, y0, x1, y1, mFgColor);
    }
    void clear();
    void setPixel(uint16_t x, uint16_t y, uint16_t color) { setPixelRaw(x, y, htobe16(color)); }
    void hLine(uint16_t x1, uint16_t x2, uint16_t y);
    void vLine(uint16_t x, uint16_t y1, uint16_t y2);
    void rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    void line(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
    void blitMonoHscan(int16_t sx, int16_t sy, int16_t w, int16_t h, const uint8_t* binData, bool bg=true);
    void blitMonoVscan(int16_t sx, int16_t sy, int16_t w, int16_t h, const uint8_t* binData, bool bg=true, int scale=1);
    void setFont(const Font& font, int8_t scale=1) { mFont = &font; mFontScale = scale; }
    bool putc(uint8_t ch, bool bg=true, uint8_t startCol=0);
    void puts(const char* str, bool bg=true);
    void gotoNextChar();
    void gotoNextLine();
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
