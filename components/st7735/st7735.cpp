#include "st7735.hpp"
#include <driver/gpio.h>
#include <esp_timer.h>
#include <driver/periph_ctrl.h>
#include <soc/spi_struct.h>
#include <esp_log.h>
#include <algorithm>
#include "stdfonts.hpp"

enum: uint8_t {
    ST77XX_NOP = 0x00,
    ST77XX_SWRESET = 0x01,
    ST77XX_RDDID = 0x04,
    ST77XX_RDDST = 0x09,
    ST77XX_SLPIN = 0x10,
    ST77XX_SLPOUT = 0x11,
    ST77XX_PTLON = 0x12,
    ST77XX_NORON = 0x13,

    ST77XX_INVOFF = 0x20,
    ST77XX_INVON = 0x21,
    ST77XX_DISPOFF = 0x28,
    ST77XX_DISPON = 0x29,
    ST77XX_CASET = 0x2A,
    ST77XX_RASET = 0x2B,
    ST77XX_RAMWR = 0x2C,
    ST77XX_RAMRD = 0x2E,

    ST77XX_PTLAR = 0x30,
    ST77XX_TEOFF = 0x34,
    ST77XX_TEON = 0x35,
    ST77XX_MADCTL = 0x36,
    ST77XX_COLMOD = 0x3A,

    ST77XX_MADCTL_MY = 0x80, ///< Bottom to top
    ST77XX_MADCTL_MX = 0x40, ///< Right to left
    ST77XX_MADCTL_MV = 0x20, ///< Reverse X and Y
    ST77XX_MADCTL_ML = 0x10, ///< LCD refresh Bottom to top
    ST77XX_MADCTL_RGB = 0x00, ///< Red-Green-Blue pixel order
    // ILI9341 specific
    ST77XX_MADCTL_BGR = 0x08, ///< Blue-Green-Red pixel order
    ST77XX_MADCTL_MH = 0x04,  ///< LCD refresh right to left

    ST77XX_RDID1 = 0xDA,
    ST77XX_RDID2 = 0xDB,
    ST77XX_RDID3 = 0xDC,
    ST77XX_RDID4 = 0xDD,

    //====
    // Some register settings
    ST7735_MADCTL_BGR = 0x08,
    ST7735_MADCTL_MH = 0x04,

    ST7735_FRMCTR1 = 0xB1,
    ST7735_FRMCTR2 = 0xB2,
    ST7735_FRMCTR3 = 0xB3,
    ST7735_INVCTR = 0xB4,
    ST7735_DISSET5 = 0xB6,

    ST7735_PWCTR1 = 0xC0,
    ST7735_PWCTR2 = 0xC1,
    ST7735_PWCTR3 = 0xC2,
    ST7735_PWCTR4 = 0xC3,
    ST7735_PWCTR5 = 0xC4,
    ST7735_VMCTR1 = 0xC5,

    ST7735_PWCTR6 = 0xFC,

    ST7735_GMCTRP1 = 0xE0,
    ST7735_GMCTRN1 = 0xE1,
};

void ST7735Display::usDelay(uint32_t us)
{
    auto end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end);
}

void ST7735Display::msDelay(uint32_t ms)
{
    usDelay(ms * 1000);
}

ST7735Display::ST7735Display(uint8_t spiHost)
:SpiMaster(spiHost)
{}

void ST7735Display::init(Coord width, Coord height, const PinCfg& pins)
{
    SpiMaster::init(pins.spi, 3);
    mWidth = width;
    mHeight = height;
    mDcPin = pins.dc;
    mRstPin = pins.rst;

    //Initialize non-SPI GPIOs
    gpio_pad_select_gpio((gpio_num_t)mDcPin);
    gpio_set_direction((gpio_num_t)mDcPin, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio((gpio_num_t)mRstPin);
    gpio_set_direction((gpio_num_t)mRstPin, GPIO_MODE_OUTPUT);

    displayReset();
}

void ST7735Display::setRstLevel(int level)
{
    gpio_set_level((gpio_num_t)mRstPin, level);
}

void ST7735Display::setDcPin(int level)
{
    gpio_set_level((gpio_num_t)mDcPin, level);
}

void ST7735Display::sendCmd(uint8_t opcode)
{
    waitDone();
    setDcPin(0);
    spiSendVal<false>(opcode);
}

void ST7735Display::prepareSendPixels()
{
    waitDone();
    setDcPin(1);
}

void ST7735Display::sendNextPixel(Color pixel)
{
    // WARNING: Requires prepareSendPixels() to have been called before
    spiSendVal(pixel);
}

void ST7735Display::sendCmd(uint8_t opcode, const std::initializer_list<uint8_t>& data)
{
    sendCmd(opcode);
    sendData(data);
}
void ST7735Display::sendData(const void* data, int size)
{
    waitDone();
    setDcPin(1);
    spiSend(data, size);
}

ST7735Display::Color ST7735Display::rgb(uint8_t R, uint8_t G, uint8_t B)
{
  // RGB565
    return ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
}

void ST7735Display::displayReset()
{
  setRstLevel(0);
  msDelay(50);
  setRstLevel(1);
  msDelay(140);
  sendCmd(ST77XX_SLPOUT);     // Sleep out, booster on
  msDelay(140);

  sendCmd(ST77XX_INVOFF);

//ST7735: sendCmd(ST77XX_MADCTL, (uint8_t)(0x08 | ST77XX_MADCTL_MX | ST77XX_MADCTL_MV))
  sendCmd(ST77XX_MADCTL, (uint8_t)(0x08 | ST77XX_MADCTL_MV));
  sendCmd(ST77XX_COLMOD, (uint8_t)0x05);

  sendCmd(ST77XX_CASET, {0x00, 0x00, 0x00, 0x7F});
  sendCmd(ST77XX_RASET, {0x00, 0x00, 0x00, 0x9F});

  sendCmd(ST77XX_NORON);   // 17: Normal display on, no args, w/delay
  clear();
  sendCmd(ST77XX_DISPON); // 18: Main screen turn on, no args, delay
  msDelay(100);
//setOrientation(kOrientNormal);
}

void ST7735Display::setOrientation(Orientation orientation)
{
    sendCmd(0x36); // Memory data access control:
    switch (orientation)
    {
        case kOrientCW:
            std::swap(mWidth, mHeight);
            sendData(0xA0); // X-Y Exchange,Y-Mirror
            break;
        case kOrientCCW:
            std::swap(mWidth, mHeight);
            sendData(0x60); // X-Y Exchange,X-Mirror
            break;
        case kOrient180:
            sendData(0xc0); // X-Mirror,Y-Mirror: Bottom to top; Right to left; RGB
            break;
        default:
            sendData(0x00); // Normal: Top to Bottom; Left to Right; RGB
            break;
    }
}

void ST7735Display::setWriteWindow(Coord XS, Coord YS, Coord w, Coord h)
{
  sendCmd(ST77XX_CASET, (uint32_t)(htobe16((uint16_t)XS) | (htobe16((uint16_t)(XS + w - 1)) << 16)));
  sendCmd(ST77XX_RASET, (uint32_t)(htobe16((uint16_t)YS) | (htobe16((uint16_t)(YS + h - 1)) << 16)));
  sendCmd(ST77XX_RAMWR); // Memory write
}
void ST7735Display::setWriteWindowCoords(Coord XS, Coord YS, Coord XE, Coord YE)
{
  sendCmd(ST77XX_CASET, (uint32_t)(htobe16((uint16_t)XS) | (htobe16((uint16_t)XE) << 16)));
  sendCmd(ST77XX_RASET, (uint32_t)(htobe16((uint16_t)YS) | (htobe16((uint16_t)YE) << 16)));
  sendCmd(ST77XX_RAMWR); // Memory write
}

void ST7735Display::fillRect(Coord x, Coord y, Coord w, Coord h, Color color)
{
    setWriteWindow(x, y, w, h);
    // TODO: If we want to support SPI bus sharing, we must lock the bus
    // before modifying the fifo buffer
    waitDone();
    int num = w * h * 2;
    int bufSize = std::min(num, (int)kMaxTransferLen);
    int numWords = (bufSize + 3) / 4;
    fifoMemset(((uint32_t)color << 16) | color, numWords);
    setDcPin(1);
    do {
        int txCount = std::min(num, bufSize);
        fifoSend(txCount);
        num -= txCount;
    } while (num > 0);
}

void ST7735Display::setPixel(Coord x, Coord y, Color color)
{
    setWriteWindowCoords(x, y, x, y);
    sendData(color);
}

void ST7735Display::hLine(Coord x1, Coord x2, Coord y)
{
    fillRect(x1, y, x2 - x1 + 1, 1);
}

void ST7735Display::vLine(Coord x, Coord y1, Coord y2)
{
    fillRect(x, y1, 1, y2 - y1 + 1);
}

void ST7735Display::line(Coord x1, Coord y1, Coord x2, Coord y2)
{
    Coord dX = x2-x1;
    Coord dY = y2-y1;

    if (dX == 0) {
        vLine(x1, y1, y2);
        return;
    }
    if (dY == 0) {
        hLine(x1, x2, y1);
        return;
    }

    Coord dXsym = (dX > 0) ? 1 : -1;
    Coord dYsym = (dY > 0) ? 1 : -1;
    dX *= dXsym;
    dY *= dYsym;
    Coord dX2 = dX << 1;
    Coord dY2 = dY << 1;
    Coord di;

    if (dX >= dY) {
        di = dY2 - dX;
        while (x1 != x2) {
            setPixel(x1, y1, mFgColor);
            x1 += dXsym;
            if (di < 0) {
                di += dY2;
            }
            else {
                di += dY2 - dX2;
                y1 += dYsym;
            }
        }
    }
    else {
        di = dX2 - dY;
        while (y1 != y2) {
            setPixel(x1, y1, mFgColor);
            y1 += dYsym;
            if (di < 0) {
                di += dX2;
            }
            else {
                di += dX2 - dY2;
                x1 += dXsym;
            }
        }
    }
    setPixel(x1, y1, mFgColor);
}

void ST7735Display::rect(Coord x1, Coord y1, Coord x2, Coord y2)
{
    hLine(x1, x2, y1);
    hLine(x1, x2, y2);
    vLine(x1, y1, y2);
    vLine(x2, y1, y2);
}

void ST7735Display::blitMonoHscan(Coord sx, Coord sy, Coord w, Coord h,
    const uint8_t* binData, int8_t bgSpacing, int scale)
{
    Coord bitW = w / scale;
    setWriteWindow(sx, sy, w + bgSpacing, h);
    prepareSendPixels();
    const uint8_t* bits = binData;
    for (int y = 0; y < h; y++) {
        uint8_t mask = 0x01;
        int rptY = 0;
        auto lineBits = bits;
        for (int x = 0; x < bitW; x++) {
            auto bit = (*bits) & mask;
            if (bit) {
                for (int rptX = 0; rptX < scale; rptX++) {
                    sendNextPixel(mFgColor);
                }
            } else {
                for (int rptX = 0; rptX < scale; rptX++) {
                    sendNextPixel(mBgColor);
                }
            }
            mask <<= 1;
            if (mask == 0) {
                mask = 0x01;
                bits++;
            }
        }
        for (int i = 0; i < bgSpacing; i++) {
            sendNextPixel(mBgColor);
        }
        if (++rptY < scale) {
            bits = lineBits;
            continue;
        }
        if (mask != 0x01) {
            bits++;
        }
    }
}

/** @param bgSpacing Draw this number of columns with background color to the right
 */
void ST7735Display::blitMonoVscan(Coord sx, Coord sy, Coord w, Coord h,
    const uint8_t* binData, int8_t bgSpacing, int scale)
{
    Coord endX = sx + w;
    if (endX > mWidth) {
        w = mWidth - sx - 1;
        if (w < 0) {
            return;
        }
        bgSpacing = 0;
    } else if (endX + bgSpacing > mWidth) {
        bgSpacing = mWidth - endX;
    }

    // scan horizontally in display RAM, but vertically in char data
    Coord bitH = h / scale;
    Coord bitW = w / scale;
    int8_t byteHeight = (bitH + 7) / 8;
    setWriteWindow(sx, sy, bitW * scale + bgSpacing, h);
    prepareSendPixels();
    int rptY = 0;
    uint8_t mask = 0x01;
    for (int y = 0; y < h; y++) {
        const uint8_t* bits = binData;
        for (int x = 0; x < bitW; x++) {
            auto color = ((*bits) & mask) ? mFgColor : mBgColor;
            for (int rptX = 0; rptX < scale; rptX++) {
                sendNextPixel(color);
            }
            bits += byteHeight;
        }
        for (int rptBg = 0; rptBg < bgSpacing; rptBg++) {
            sendNextPixel(mBgColor);
        }
        if (++rptY < scale) {
            continue;
        }
        rptY = 0;
        mask <<= 1;
        if (mask == 0) {
            mask = 0x01;
            binData++;
        }
    }
}

bool ST7735Display::putc(uint8_t ch, uint8_t flags, uint8_t startCol)
{
    if (!mFont) {
        return false;
    }
    if (cursorY > mHeight) {
        return false;
    }
    uint8_t width = ch;
    // returns char width via the charcode argument
    auto charData = mFont->getCharData(width);
    if (!charData) {
        return false;
    }
    auto height = mFont->height * mFontScale;
    int charSpc = mFont->charSpacing;
    if (startCol) { // start drawing the char from the specified column
        if (startCol >= width) { // column is beyond char width
            // check if we still need to draw the spacing after that invisible char
            // needed for easier handling of scrolling text
            auto spacingToDraw = width + charSpc - startCol;
            if (spacingToDraw > charSpc) {
                return false;
            }
            spacingToDraw *= mFontScale; // but column is within char spacing
            if (cursorX + spacingToDraw > mWidth) {
                spacingToDraw = mWidth - cursorX;
                if (spacingToDraw <= 0) {
                    return false;
                }
            }
            clear(cursorX, cursorY, spacingToDraw, height);
            cursorX += spacingToDraw;
            return true;
        }
        auto byteHeight = (mFont->height + 7) / 8;
        charData += byteHeight * startCol; // skip first columns
        width -= startCol;
    }
    width *= mFontScale;
    charSpc *= mFontScale;

    // we need to calculate new cursor X in order to determine if we
    // should increment cursorY. That's why we do the newCursorX gymnastics
    Coord newCursorX = cursorX + width + charSpc;
    if (newCursorX > mWidth) {
        if (cursorX < mWidth && (flags & kFlagAllowPartial)) {
            newCursorX = mWidth;
        } else {
            if (flags & kFlagNoAutoNewline) {
                return false;
            }
            cursorX = 0;
            cursorY += height + mFont->lineSpacing * mFontScale;
            newCursorX = width + charSpc;
        }
    }
    if (mFont->isVertScan) {
        blitMonoVscan(cursorX, cursorY, width, height, charData, charSpc, mFontScale);
    } else {
        blitMonoHscan(cursorX, cursorY, width, height, charData, charSpc, mFontScale);
    }
    cursorX = newCursorX;
    return true;
}

void ST7735Display::puts(const char* str, uint8_t flags)
{
    char ch;
    while((ch = *(str++))) {
        if (ch == '\n') {
            cursorX = 0;
            cursorY += (mFont->height + mFont->lineSpacing) * mFontScale;
        } else if (ch != '\r') {
            putc(ch, flags);
        }
    }
}

void ST7735Display::nputs(const char* str, int len, uint8_t flags)
{
    auto end = str + len;
    while(str < end) {
        char ch = *(str++);
        if (!ch) {
            return;
        }
        if (ch == '\n') {
            cursorX = 0;
            cursorY += (mFont->height + mFont->lineSpacing) * mFontScale;
        } else if (ch != '\r') {
            putc(ch, flags);
        }
    }
}
int ST7735Display::textWidth(const char *str)
{
    if (mFont->isMono()) {
        return (mFont->width + mFont->charSpacing) * mFontScale * strlen(str);
    } else {
        int w = 0;
        for (const char* p = str; *p; p++) {
            w += mFontScale * mFont->charWidth(*p);
        }
        return w;
    }
}
void ST7735Display::putsCentered(const char *str, int reserveRight)
{
    int padding = (mWidth - cursorX - reserveRight - textWidth(str)) / 2;
    if (padding < 0) {
        padding = 0;
    }
    cursorX += padding;
    puts(str, kFlagNoAutoNewline);
}
void ST7735Display::gotoNextChar()
{
    cursorX += mFont->width * mFontScale + mFont->charSpacing;
}
void ST7735Display::gotoNextLine()
{
    cursorY += mFont->height * mFontScale + mFont->lineSpacing;
}
