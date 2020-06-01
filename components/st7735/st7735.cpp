#include "st7735.hpp"
#include <driver/gpio.h>
#include <esp_timer.h>
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

    ST77XX_MADCTL_MY = 0x80,
    ST77XX_MADCTL_MX = 0x40,
    ST77XX_MADCTL_MV = 0x20,
    ST77XX_MADCTL_ML = 0x10,
    ST77XX_MADCTL_RGB = 0x00,

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

ST7735Display::ST7735Display()
{
    memset(&mTrans, 0, sizeof(mTrans));
    mTrans.user = this;
}

void ST7735Display::init(int16_t width, int16_t height, PinCfg& pins)
{
    mWidth = width;
    mHeight = height;
    mDcPin = pins.dc;
    mRstPin = pins.rst;

    //Initialize non-SPI GPIOs
    gpio_pad_select_gpio((gpio_num_t)mDcPin);
    gpio_set_direction((gpio_num_t)mDcPin, GPIO_MODE_OUTPUT);
    gpio_pad_select_gpio((gpio_num_t)mRstPin);
    gpio_set_direction((gpio_num_t)mRstPin, GPIO_MODE_OUTPUT);
    // init SPI bus and device
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    spi_bus_config_t buscfg = {
        .mosi_io_num=pins.mosi,
        .miso_io_num=-1,
        .sclk_io_num=pins.clk,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz = width * 2 * 4 // 4 lines
    };
    //Initialize the SPI bus
    auto ret = spi_bus_initialize(pins.spiHost, &buscfg, 1);
    ESP_ERROR_CHECK(ret);

    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = SPI_MASTER_FREQ_26M,
        .spics_io_num = pins.cs,
        .queue_size = 4,
        .pre_cb = preTransferCallback  //Specify pre-transfer callback to handle D/C line
    };
    //Attach the LCD to the SPI bus
    ret = spi_bus_add_device(pins.spiHost, &devcfg, &mSpi);
    ESP_ERROR_CHECK(ret);

    displayReset();
}

void ST7735Display::setRstLevel(int level)
{
    gpio_set_level((gpio_num_t)mRstPin, level);
}

void ST7735Display::sendCmd(uint8_t opcode)
{
    mTrans.flags = SPI_TRANS_USE_TXDATA;   //D/C needs to be set to 0
    mTrans.length = 8;                     //Command is 8 bits
    mTrans.tx_data[0] = opcode;            //The data is the cmd itself
    mTrans.cmd = 0;
    esp_err_t ret = spi_device_polling_transmit(mSpi, &mTrans);  //Transmit!
    assert(ret == ESP_OK);            //Should have had no issues.
}

void ST7735Display::sendData(const uint8_t* data, int len)
{
    if (len == 0) {
        return;
    }

    mTrans.flags = 0;
    mTrans.length = len << 3;             //Len is in bytes, transaction length is in bits.
    mTrans.tx_buffer = data;             //Data
    mTrans.cmd = 1; // D/C needs to be set to 1

    esp_err_t ret = spi_device_polling_transmit(mSpi, &mTrans);  //Transmit!
    assert(ret == ESP_OK);            //Should have had no issues.
}

void ST7735Display::prepareSendPixels()
{
    mTrans.flags = 0;
    mTrans.length = 16;
    mTrans.cmd = 1; // D/C needs to be set to 1
}

void ST7735Display::sendNextPixel(uint16_t pixel)
{
    // WARNING: Requires prepareSendPixels() to have been called before
    mTrans.tx_buffer = &pixel;
    spi_device_polling_transmit(mSpi, &mTrans);
}

void ST7735Display::sendCmd(uint8_t opcode, const std::initializer_list<uint8_t>& data)
{
    sendCmd(opcode);
    sendData(data);
}

IRAM_ATTR void ST7735Display::preTransferCallback(spi_transaction_t *t)
{
    auto self = static_cast<ST7735Display*>(t->user);
    gpio_set_level((gpio_num_t)self->mDcPin, t->cmd);
}

uint16_t ST7735Display::mkcolor(uint8_t R,uint8_t G,uint8_t B)
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
  sendCmd(ST77XX_MADCTL, { 0x08 | ST77XX_MADCTL_MX | ST77XX_MADCTL_MV });
  sendCmd(ST77XX_COLMOD, {0x05});

  sendCmd(ST77XX_CASET, {0x00, 0x00, 0x00, 0x7F});
  sendCmd(ST77XX_RASET, {0x00, 0x00, 0x00, 0x9F});

  sendCmd(ST77XX_NORON);   // 17: Normal display on, no args, w/delay
  clear(0x0000);
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
            sendData({0xA0}); // X-Y Exchange,Y-Mirror
            break;
        case kOrientCCW:
            std::swap(mWidth, mHeight);
            sendData({0x60}); // X-Y Exchange,X-Mirror
            break;
        case kOrient180:
            sendData({0xc0}); // X-Mirror,Y-Mirror: Bottom to top; Right to left; RGB
            break;
        default:
            sendData({0x00}); // Normal: Top to Bottom; Left to Right; RGB
            break;
    }
}

void ST7735Display::setWriteWindow(uint16_t XS, uint16_t YS, uint16_t XE, uint16_t YE)
{
  sendCmd(ST77XX_CASET, {
      (uint8_t)(XS >> 8),
      (uint8_t)XS,
      (uint8_t)(XE >> 8),
      (uint8_t)XE
  });

  sendCmd(ST77XX_RASET, {
      (uint8_t)(YS >> 8),
      (uint8_t)(YS),
      (uint8_t)(YE >> 8),
      (uint8_t)(YE)
   });

  sendCmd(ST77XX_RAMWR); // Memory write
}

void ST7735Display::fillRect(int16_t x0, int16_t y0, int16_t x1,
                             int16_t y1, uint16_t color)
{
    enum { kBufSize = 128 };
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    if (y0 > y1) {
        std::swap(y0, y1);
    }
    uint8_t ch = color >> 8;
    uint8_t cl = (uint8_t)color;

    setWriteWindow(x0, y0, x1, y1);
    int num = (x1 - x0 + 1) * (y1 - y0 + 1) * 2;
    int bufSize = std::min(num, (int)kBufSize);
    uint8_t* txbuf = (uint8_t*)alloca(bufSize);
    auto bufEnd = txbuf + bufSize;
    for (auto ptr = txbuf; ptr < bufEnd;) {
        *(ptr++) = ch;
        *(ptr++) = cl;
    }
    while (num > 0) {
        int txCount = std::min(num, bufSize);
        sendData(txbuf, txCount);
        num -= txCount;
    }
}

void ST7735Display::clear(uint16_t color)
{
    fillRect(0, 0, mWidth-1, mHeight-1, color);
}

void ST7735Display::setPixel(uint16_t x, uint16_t y, uint16_t color)
{
    setWriteWindow(x, y, x, y);
    sendData({(uint8_t)(color >> 8), (uint8_t)color});
}

void ST7735Display::hLine(uint16_t x1, uint16_t x2, uint16_t y, uint16_t color)
{
    fillRect(x1, y, x2, y, color);
}

void ST7735Display::vLine(uint16_t x, uint16_t y1, uint16_t y2, uint16_t color)
{
    fillRect(x, y1, x, y2, color);
}

void ST7735Display::line(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color)
{
    int16_t dX = x2-x1;
    int16_t dY = y2-y1;

    if (dX == 0) {
        vLine(x1, y1, y2, color);
        return;
    }
    if (dY == 0) {
        hLine(x1, x2, y1, color);
        return;
    }

    int16_t dXsym = (dX > 0) ? 1 : -1;
    int16_t dYsym = (dY > 0) ? 1 : -1;
    dX *= dXsym;
    dY *= dYsym;
    int16_t dX2 = dX << 1;
    int16_t dY2 = dY << 1;
    int16_t di;

    if (dX >= dY) {
        di = dY2 - dX;
        while (x1 != x2) {
            setPixel(x1, y1, color);
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
            setPixel(x1, y1, color);
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
    setPixel(x1, y1, color);
}

void ST7735Display::rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    hLine(x1, x2, y1, color);
    hLine(x1, x2, y2, color);
    vLine(x1, y1, y2, color);
    vLine(x2, y1, y2, color);
}

void ST7735Display::blitMonoHscan(int16_t sx, int16_t sy, int16_t w, int16_t h, const uint8_t* binData, bool bg)
{
    setWriteWindow(sx, sy, sx+w-1, sy+h-1);
    prepareSendPixels();
    const uint8_t* bits = binData;
    uint8_t mask = 0x80;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            auto bit = (*bits) & mask;
            if (bit) {
                sendNextPixel(mFgColor);
            } else if (bg) {
                sendNextPixel(mBgColor);
            }
            mask >>= 1;
            if (mask == 0) {
                mask = 0x80;
                bits++;
            }
        }
        if (mask != 0x80) {
            bits++;
            mask = 0x80;
        }
    }
}
void ST7735Display::blitMonoVscan(int16_t sx, int16_t sy, int16_t w, int16_t h,
    const uint8_t* binData, bool bg, int scale)
{
    // scan horizontally in display RAM, but vertically in char data
    int16_t bitH = h / scale;
    int16_t bitW = w / scale;
    int8_t byteHeight = (bitH + 7) / 8;
    setWriteWindow(sx, sy, (sx+w-1), (sy+h-1));
    prepareSendPixels();
    int rptY = 0;
    uint8_t mask = 0x01;
    for (int y = 0; y < h; y++) {
        const uint8_t* bits = binData;
        for (int x = 0; x < bitW; x++) {
            auto fg = (*bits) & mask;
            if (fg) {
                for (int rptX = 0; rptX < scale; rptX++) {
                    sendNextPixel(mFgColor);
                }
            } else if (bg) {
                for (int rptX = 0; rptX < scale; rptX++) {
                    sendNextPixel(mBgColor);
                }
            }
            bits += byteHeight;
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

bool ST7735Display::putc(uint8_t ch, bool bg, uint8_t startCol)
{
    if (cursorY > mHeight) {
        return false;
    }
    uint8_t width = ch;
    auto charData = mFont->getCharData(width);
    if (!charData) {
        return false;
    }
    if (startCol) {
        if (startCol >= width) {
            return false;
        }
        auto byteHeight = (mFont->height + 7) / 8;
        charData += byteHeight * startCol; // skip first columns
        width -= startCol;
    }

    width *= mFontScale;
    auto height = mFont->height * mFontScale;
    auto newCursorX = cursorX + width + mFont->charSpacing * mFontScale;
    if (newCursorX >= mWidth) {
        cursorX = 0;
        cursorY += height + mFont->lineSpacing * mFontScale;
        newCursorX = width + mFont->charSpacing * mFontScale;
    }
    blitMonoVscan(cursorX, cursorY, width, height, charData, bg, mFontScale);

    cursorX = newCursorX;
    return true;
}

void ST7735Display::puts(const char* str, bool bg)
{
    while(*str) {
        putc(*(str++), bg);
    }
}

void ST7735Display::gotoNextChar()
{
    cursorX += mFont->width * mFontScale + mFont->charSpacing;
}
void ST7735Display::gotoNextLine()
{
    cursorY += mFont->height * mFontScale + mFont->lineSpacing;
}
