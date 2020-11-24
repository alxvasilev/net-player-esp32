#ifndef __ST7735_H__
#define __ST7735_H__

#include <string.h>
#include <stdint.h>
#include <endian.h>
#include <initializer_list>
#include "spi.hpp"
#include "stdfonts.hpp"

class ST7735Display: public SpiMaster
{
public:
    struct PinCfg
    {
        SpiPinCfg spi;
        uint8_t dc; // data/command
        uint8_t rst;
    };
    enum { kMaxTransferLen = 64 };
    typedef int16_t Coord;
    typedef uint16_t Color;
    enum Orientation
    {
      kOrientNormal = 0,
      kOrientCW     = 1,
      kOrientCCW    = 2,
      kOrient180    = 3
    };
    enum DrawFlags
    {
        kFlagNoAutoNewline = 1,
        kFlagAllowPartial = 2
    };
protected:
    Coord mWidth;
    Coord mHeight;
    uint8_t mRstPin;
    uint8_t mDcPin;
    Color mBgColor = 0x0000;
    Color mFgColor = 0xffff;
    const Font* mFont = &Font_5x7;
    uint8_t mFontScale = 1;
    void setRstLevel(int level);
    void setDcPin(int level);
    inline void execTransaction();
    void displayReset();
public:
    Coord cursorX = 0;
    Coord cursorY = 0;
    Coord width() const { return mWidth; }
    Coord height() const { return mHeight; }
    const Font* font() const { return mFont; }
    static void usDelay(uint32_t us);
    static void msDelay(uint32_t ms);
    static Color rgb(uint8_t R, uint8_t G, uint8_t B);
    ST7735Display(uint8_t spiHost);
    Color fgColor() const { return mFgColor; }
    Color bgColor() const { return mBgColor; }
    void setFgColor(Color color) { mFgColor = htobe16(color); }
    void setFgColor(uint8_t r, uint8_t g, uint8_t b) { setFgColor(rgb(r, g, b)); }
    void setBgColor(Color color) { mBgColor = htobe16(color); }
    void setBgColor(uint8_t r, uint8_t g, uint8_t b) { setBgColor(rgb(r, g, b)); }

    void gotoXY(Coord x, Coord y) { cursorX = x; cursorY = y; }
    void init(Coord width, Coord height, const PinCfg& pins);
    void sendCmd(uint8_t opcode);
    void sendCmd(uint8_t opcode, const std::initializer_list<uint8_t>& data);
    template <typename T>
    void sendCmd(uint8_t opcode, T data)
    {
        waitDone();
        sendCmd(opcode);
        sendData(data);
    }

    void sendData(const void* data, int size);
    void sendData(const std::initializer_list<uint8_t>& data) {
        sendData(data.begin(), (int)data.size());
    }
    template<typename T>
    void sendData(T data)
    {
        waitDone();
        setDcPin(1);
        spiSendVal(data);
    }
    void prepareSendPixels();
    void sendNextPixel(Color pixel);
    void setOrientation(Orientation orientation);
    void setWriteWindow(Coord XS, Coord YS, Coord w, Coord h);
    void setWriteWindowCoords(Coord XS, Coord YS, Coord XE, Coord YE);
    void fillRect(Coord x, Coord y, Coord w, Coord h, Color color);
    void fillRect(Coord x, Coord y, Coord w, Coord h) { fillRect(x, y, w, h, mFgColor); }
    void clear() { fillRect(0, 0, mWidth, mHeight, mBgColor); }
    void clear(Coord x, Coord y, Coord w, Coord h) { fillRect(x, y, w, h, mBgColor); }
    void setPixel(Coord x, Coord y, Color color);
    void hLine(Coord x1, Coord x2, Coord y);
    void vLine(Coord x, Coord y1, Coord y2);
    void rect(Coord x1, Coord y1, Coord x2, Coord y2);
    void line(Coord x1, Coord y1, Coord x2, Coord y2);
    void blitMonoHscan(Coord sx, Coord sy, Coord w, Coord h, const uint8_t* binData, int8_t bgSpacing=0, int scale=1);
    void blitMonoVscan(Coord sx, Coord sy, Coord w, Coord h, const uint8_t* binData, int8_t bgSpacing=0, int scale=1);
    void setFont(const Font& font, int8_t scale=1) { mFont = &font; mFontScale = scale; }
    void setFontScale(int8_t scale) { mFontScale = scale; }
    Coord fontHeight() const { return mFont->height * mFontScale; }
    Coord fontWidth() const { return mFont->width * mFontScale; }
    int8_t charWidth(char ch=0) const { return (mFont->charWidth(ch) + mFont->charSpacing) * mFontScale; }
    int8_t charHeight() const { return (mFont->height + mFont->lineSpacing) * mFontScale; }
    int8_t charsPerLine() const { return mWidth / charWidth(); }
    void skipCharsX(int n) { cursorX += textWidth(n); }
    bool putc(uint8_t ch, uint8_t flags = 0, uint8_t startCol=0);
    void puts(const char* str, uint8_t flags = 0);
    void nputs(const char* str, int len, uint8_t flag=0);
    void putsCentered(const char* str, int reserveRight=0);
    int textWidth(int charCnt) const { return charCnt * (mFont->width + mFont->charSpacing); }
    int textWidth(const char* str);
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
