#include "st7735.hpp"
#include <driver/gpio.h>
#include <esp_timer.h>

#define ST77XX_NOP 0x00
#define ST77XX_SWRESET 0x01
#define ST77XX_RDDID 0x04
#define ST77XX_RDDST 0x09

#define ST77XX_SLPIN 0x10
#define ST77XX_SLPOUT 0x11
#define ST77XX_PTLON 0x12
#define ST77XX_NORON 0x13

#define ST77XX_INVOFF 0x20
#define ST77XX_INVON 0x21
#define ST77XX_DISPOFF 0x28
#define ST77XX_DISPON 0x29
#define ST77XX_CASET 0x2A
#define ST77XX_RASET 0x2B
#define ST77XX_RAMWR 0x2C
#define ST77XX_RAMRD 0x2E

#define ST77XX_PTLAR 0x30
#define ST77XX_TEOFF 0x34
#define ST77XX_TEON 0x35
#define ST77XX_MADCTL 0x36
#define ST77XX_COLMOD 0x3A

#define ST77XX_MADCTL_MY 0x80
#define ST77XX_MADCTL_MX 0x40
#define ST77XX_MADCTL_MV 0x20
#define ST77XX_MADCTL_ML 0x10
#define ST77XX_MADCTL_RGB 0x00

#define ST77XX_RDID1 0xDA
#define ST77XX_RDID2 0xDB
#define ST77XX_RDID3 0xDC
#define ST77XX_RDID4 0xDD

//====
// Some register settings
#define ST7735_MADCTL_BGR 0x08
#define ST7735_MADCTL_MH 0x04

#define ST7735_FRMCTR1 0xB1
#define ST7735_FRMCTR2 0xB2
#define ST7735_FRMCTR3 0xB3
#define ST7735_INVCTR 0xB4
#define ST7735_DISSET5 0xB6

#define ST7735_PWCTR1 0xC0
#define ST7735_PWCTR2 0xC1
#define ST7735_PWCTR3 0xC2
#define ST7735_PWCTR4 0xC3
#define ST7735_PWCTR5 0xC4
#define ST7735_VMCTR1 0xC5

#define ST7735_PWCTR6 0xFC

#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1

void ST7735Display::usDelay(uint32_t us)
{
    auto end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end);
}
void ST7735Display::msDelay(uint32_t ms)
{
    usDelay(ms * 1000);
}

void ST7735Display::init(int16_t width, int16_t height, PinCfg& pins)
{
    mWidth = width;
    mHeight = height;
    mDcPin = pins.dc;
    mRstPin = pins.rst;
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    spi_bus_config_t buscfg = {
        .mosi_io_num=pins.mosi,
        .miso_io_num=-1,
        .sclk_io_num=pins.clk,
        .quadwp_io_num=-1,
        .quadhd_io_num=-1,
        .max_transfer_sz = width * 2 * 4 // 4 lines
    };
    spi_device_interface_config_t devcfg = {
        .mode=0,                                //SPI mode 0
        .clock_speed_hz=10 * 1000 * 1000,       //Clock out at 10 MHz
        .spics_io_num=pins.cs,                  //CS pin
        .queue_size=7,                          //We want to be able to queue 7 transactions at a time
        .pre_cb = preTransferCallback           //Specify pre-transfer callback to handle D/C line
    };
    //Initialize the SPI bus
    auto ret = spi_bus_initialize(pins.spiHost, &buscfg, 1);
    ESP_ERROR_CHECK(ret);
    //Attach the LCD to the SPI bus
    ret = spi_bus_add_device(pins.spiHost, &devcfg, &mSpi);
    ESP_ERROR_CHECK(ret);

    //Initialize non-SPI GPIOs
    gpio_set_direction((gpio_num_t)mDcPin, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)mRstPin, GPIO_MODE_OUTPUT);
    displayReset();
}

void ST7735Display::setRstLevel(int level)
{
    gpio_set_level((gpio_num_t)mRstPin, level);
}

void ST7735Display::sendCmd(uint8_t opcode)
{
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));         //Zero out the transaction
    t.flags = SPI_TRANS_USE_TXDATA;   //D/C needs to be set to 0
    t.length = 8;                     //Command is 8 bits
    t.tx_data[0] = opcode;            //The data is the cmd itself
    t.user = this;
    ret=spi_device_polling_transmit(mSpi, &t);  //Transmit!
    assert(ret == ESP_OK);            //Should have had no issues.
}

void ST7735Display::sendData(const uint8_t* data, int len)
{
    if (len == 0) {
        return;
    }
    esp_err_t ret;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));       //Zero out the transaction
    t.length = len * 8;             //Len is in bytes, transaction length is in bits.
    t.tx_buffer = data;             //Data
    t.user = this;
    t.flags = SPI_TRANS_SET_CD;       //D/C needs to be set to 1
    ret = spi_device_polling_transmit(mSpi, &t);  //Transmit!
    assert(ret == ESP_OK);            //Should have had no issues.
}
void ST7735Display::sendData(const std::vector<uint8_t>& data)
{
    sendData(data.data(), data.size());
}

void ST7735Display::preTransferCallback(spi_transaction_t *t)
{
    auto self = static_cast<ST7735Display*>(t->user);
    gpio_set_level((gpio_num_t)self->mDcPin, (t->flags & SPI_TRANS_SET_CD) ? 1 : 0);
}

uint16_t ST7735Display::mkcolor(uint8_t R,uint8_t G,uint8_t B)
{
  // RGB565
    return ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
}

void ST7735Display::displayReset()
{
  setRstLevel(1);
  msDelay(100);
  setRstLevel(0);
  msDelay(100);
  setRstLevel(1);
  msDelay(100);

  sendCmd(ST77XX_SWRESET);
  msDelay(50);

  sendCmd(ST77XX_SLPOUT);     // Sleep out, booster on
  msDelay(500);

  sendCmd(ST77XX_COLMOD),
  sendData({0x05});
  msDelay(10);

  sendCmd(ST7735_FRMCTR1);     // Frame rate control
  sendData({
      0x00,   //     fastest refresh
      0x06,   //     6 lines front porch
      0x03,   //     3 lines back porch
  });
  msDelay(10);

  sendCmd(ST77XX_MADCTL);
  sendData({0x08});
  sendCmd(ST7735_DISSET5);
  sendData({
      0x15,  // 1 clk cycle nonoverlap, 2 cycle gate rise, 3 cycle osc equalize
      0x02   // Fix on VTL
  });
  sendCmd(ST7735_INVCTR); // inversion control
  sendData({0x00}); // Line inversion
  sendCmd(ST7735_PWCTR1); // power control
  sendData({
      0x02, //     GVDD = 4.7V
      0x70  //     1.0uA
  });
  msDelay(10);
  sendCmd(ST7735_PWCTR2);
  sendData({0x05}); //     VGH = 14.7V, VGL = -7.35V
  sendCmd(ST7735_PWCTR3);
  sendData({
      0x01, //     Opamp current small
      0x02, //     Boost frequency
  });
  sendCmd(ST7735_VMCTR1);
  sendData({
      0x3C, //     VCOMH = 4V
      0x38  //     VCOML = -1.1V
  });
  msDelay(10);

  sendCmd(ST7735_PWCTR6);
  sendData({0x11, 0x15});

  sendCmd(ST7735_GMCTRP1); // Gamma Adjustments (pos. polarity), 16 args + delay:
  sendData({
      0x09, 0x16, 0x09, 0x20,       //     (Not entirely necessary, but provides
      0x21, 0x1B, 0x13, 0x19,       //      accurate colors)
      0x17, 0x15, 0x1E, 0x2B,
      0x04, 0x05, 0x02, 0x0E
  });
  sendCmd(ST7735_GMCTRN1); // Gamma Adjustments (neg. polarity), 16 args + delay:
  sendData({
      0x0B, 0x14, 0x08, 0x1E,       //     (Not entirely necessary, but provides
      0x22, 0x1D, 0x18, 0x1E,       //      accurate colors)
      0x1B, 0x1A, 0x24, 0x2B,
      0x06, 0x06, 0x02, 0x0F
  });
  msDelay(10);

  sendCmd(ST77XX_CASET);   // 15: Column addr set, 4 args, no delay:
  sendData({
    0x00, 0x02,            //     XSTART = 2
    0x00, 0x81             //     XEND = 129
  });

  sendCmd(ST77XX_RASET);   // 16: Row addr set, 4 args, no delay:
  sendData({
    0x00, 0x02,            //     XSTART = 1
    0x00, 0x81,            //     XEND = 160
  });
  sendCmd(ST77XX_NORON);   // 17: Normal display on, no args, w/delay
  msDelay(10);

  sendCmd(ST77XX_DISPON); // 18: Main screen turn on, no args, delay
  msDelay(500);
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
  sendCmd(0x2a); // Column address set
  sendData({
      (uint8_t)(XS >> 8),
      (uint8_t)XS,
      (uint8_t)(XE >> 8),
      (uint8_t)XE
  });

  sendCmd(0x2b); // Row address set
  sendData({
      (uint8_t)(YS >> 8),
      (uint8_t)YS,
      (uint8_t)(YE >> 8),
      (uint8_t)YE
   });

  sendCmd(0x2c); // Memory write
}

void ST7735Display::fillRect(int16_t x0, int16_t y0, int16_t x1,
                             int16_t y1, uint16_t color)
{
    if (x0 > x1) {
        std::swap(x0, x1);
    }
    if (y0 > y1) {
        std::swap(y0, y1);
    }
    uint8_t ch = color >> 8;
    uint8_t cl = (uint8_t)color;

    setWriteWindow(x0, y0, x1, y1);
    int num = (x1 - x0 + 1) * (y1 - y0 + 1);
    int bufSize = std::min(num * 2, 128);
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
    int16_t dXsym = (dX > 0) ? 1 : -1;
    int16_t dYsym = (dY > 0) ? 1 : -1;

    if (dX == 0) {
        vLine(x1, y1, y2, color);
        return;
    }
    if (dY == 0) {
        hLine(x1, x2, y1, color);
        return;
    }

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
/*
void ST7735Display::putChar5x7(uint8_t scale, uint16_t X, uint16_t Y, uint8_t chr, uint16_t color, uint16_t bgcolor)
{
  uint16_t i,j;
  uint8_t buffer[5];
  color = htons(color);
  bgcolor = htons(bgcolor);

  if ((chr >= 0x20) && (chr <= 0x7F))
  {
    // ASCII[0x20-0x7F]
    memcpy(buffer,&Font5x7[(chr - 32) * 5], 5);
  }
  else if (chr >= 0xA0)
  {
    // CP1251[0xA0-0xFF]
    memcpy(buffer,&Font5x7[(chr - 64) * 5], 5);
  }
  else
  {
    // unsupported symbol
    memcpy(buffer,&Font5x7[160], 5);
  }

  // scale equals 1 drawing faster
  if (scale == 1) {
      setWriteWindow(X, Y, X + 5, Y + 7);
      for (j = 0; j < 7; j++) {
          for (i = 0; i < 5; i++) {
              writeData(((buffer[i] >> j) & 0x01) ? &color : &bgcolor, 2);
          }
          // vertical spacing
          writeData(&bgcolor, 2);
      }

    // horizontal spacing
    for (i = 0; i < 6; i++)
    {
      ST7735_write(BCH);
      ST7735_write(BCL);
    }
  }
  else
  {
    A0_H();
    for (j = 0; j < 7; j++)
    {
      for (i = 0; i < 5; i++)
      {
        // pixel group
        ST7735_FillRect(X + (i * scale), Y + (j * scale), X + (i * scale) + scale - 1, Y + (j * scale) + scale - 1, ((buffer[i] >> j) & 0x01) ? color : bgcolor);
      }
      // vertical spacing
//      ST7735_FillRect(X + (i * scale), Y + (j * scale), X + (i * scale) + scale - 1, Y + (j * scale) + scale - 1, V_SEP);
      ST7735_FillRect(X + (i * scale), Y + (j * scale), X + (i * scale) + scale - 1, Y + (j * scale) + scale - 1, bgcolor);
    }
    // horizontal spacing
//    ST7735_FillRect(X, Y + (j * scale), X + (i * scale) + scale - 1, Y + (j * scale) + scale - 1, H_SEP);
    ST7735_FillRect(X, Y + (j * scale), X + (i * scale) + scale - 1, Y + (j * scale) + scale - 1, bgcolor);
  }
  CS_H();
}

void ST7735_PutStr5x7(uint8_t scale, uint8_t X, uint8_t Y, char *str, uint16_t color, uint16_t bgcolor)
{
  // scale equals 1 drawing faster
  if (scale == 1)
  {
    while (*str)
    {
      ST7735_PutChar5x7(scale, X,Y,*str++,color,bgcolor);
      if (X < scr_width - 6) { X += 6; } else if (Y < scr_height - 8) { X = 0; Y += 8; } else { X = 0; Y = 0; }
    };
  }
  else
  {
    while (*str)
    {
      ST7735_PutChar5x7(scale, X,Y,*str++,color,bgcolor);
      if (X < scr_width - (scale*5) + scale) { X += (scale * 5) + scale; } else if (Y < scr_height - (scale * 7) + scale) { X = 0; Y += (scale * 7) + scale; } else { X = 0; Y = 0; }
    };
  }
}

void ST7735_PutChar7x11(uint16_t X, uint16_t Y, uint8_t chr, uint16_t color, uint16_t bgcolor)
{
  uint16_t i,j;
  uint8_t buffer[11];
  uint8_t CH = color >> 8;
  uint8_t CL = (uint8_t)color;
  uint8_t BCH = bgcolor >> 8;
  uint8_t BCL = (uint8_t)bgcolor;

  if ((chr >= 0x20) && (chr <= 0x7F))
  {
    // ASCII[0x20-0x7F]
    memcpy(buffer,&Font7x11[(chr - 32) * 11], 11);
  }
  else if (chr >= 0xA0)
  {
    // CP1251[0xA0-0xFF]
    memcpy(buffer,&Font7x11[(chr - 64) * 11], 11);
  }
  else
  {
    // unsupported symbol
    memcpy(buffer,&Font7x11[160], 11);
  }

  CS_L();
  ST7735_AddrSet(X, Y, X + 7, Y + 11);
  A0_H();
  for (i = 0; i < 11; i++)
  {
    for (j = 0; j < 7; j++)
    {
      if ((buffer[i] >> j) & 0x01)
      {
        ST7735_write(CH);
        ST7735_write(CL);
      }
      else
      {
        ST7735_write(BCH);
        ST7735_write(BCL);
      }
    }
    // vertical spacing
    ST7735_write(BCH);
    ST7735_write(BCL);
  }

  // horizontal spacing
  for (i = 0; i < 8; i++)
  {
    ST7735_write(BCH);
    ST7735_write(BCL);
  }

  CS_H();
}

void ST7735_PutStr7x11(uint8_t X, uint8_t Y, char *str, uint16_t color, uint16_t bgcolor)
{
  while (*str)
  {
    ST7735_PutChar7x11(X,Y,*str++,color,bgcolor);
    if (X < scr_width - 8) { X += 8; } else if (Y < scr_height - 12) { X = 0; Y += 12; } else { X = 0; Y = 0; }
  };
}
*/
