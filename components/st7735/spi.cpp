#include "spi.hpp"
#include <driver/gpio.h>
#include <soc/spi_reg.h>
#include <soc/spi_periph.h>
#include <driver/periph_ctrl.h>
#include <driver/spi_common_internal.h>
#include <soc/dport_reg.h>
#include <algorithm>
SpiMaster::SpiMaster(uint8_t spiHost)
:mSpiHost(spiHost), mReg(*(volatile spi_dev_t *)(DR_REG_SPI3_BASE))//, mReg(*(volatile spi_dev_t*)(REG_SPI_BASE(spiHost)))
{
}

void SpiMaster::init(const SpiPinCfg& pins, int freqDiv)
{
    // init SPI bus and device
//  periph_module_enable(spi_periph_signal[mSpiHost].module);
    spicommon_periph_claim((spi_host_device_t)mSpiHost, "SpiMaster");
    configPins(pins);

    /*
    if (mSpiHost == HSPI_HOST) {
          DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI2_CLK_EN);
          DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI2_RST);
    } else if(mSpiHost == VSPI_HOST) {
          DPORT_SET_PERI_REG_MASK(DPORT_PERIP_CLK_EN_REG, DPORT_SPI3_CLK_EN);
          DPORT_CLEAR_PERI_REG_MASK(DPORT_PERIP_RST_EN_REG, DPORT_SPI3_RST);
    } else {
        assert(false);
    }
    */

    // Set clock - ref manual secton 7.4
    if (freqDiv <= 1) {
        mReg.clock.clk_equ_sysclk = 1;
    } else {
        mReg.clock.clk_equ_sysclk = 0;
        mReg.clock.clkdiv_pre = (freqDiv < 64) ? 0 : (freqDiv / 64) - 1;
        mReg.clock.clkcnt_n = freqDiv - 1;
        mReg.clock.clkcnt_l = freqDiv - 1;
        mReg.clock.clkcnt_h = (freqDiv / 2) - 1;
    }
    // set as master
    mReg.slave.slave_mode = 0;

    // bit order setup - big endian
    mReg.ctrl.wr_bit_order = 0;
    mReg.ctrl.rd_bit_order = 0;
    // cs setup
    mReg.user.cs_setup = 1;
    mReg.user.cs_hold = 1;
    mReg.ctrl2.hold_time = 1;
    mReg.ctrl2.setup_time = 1;

    // disable command, address and dummy phases
    mReg.user.usr_command = 0;
    mReg.user2.usr_command_bitlen = 0;
    mReg.user.usr_addr = 0;
    mReg.user1.usr_addr_bitlen = 0;
    mReg.user.usr_dummy = 0;
    mReg.user.usr_dummy_idle = 0;
    mReg.user1.val = 0; // just in case, zero address and dummy params

    // disable read, enable write
    mReg.user.usr_miso = 0;
    mReg.user.usr_mosi = 1;
    //use high half of data registers for tx
    mReg.user.usr_mosi_highpart = 1;

    // mode 0 - table 27 in the Reference Manual
    mReg.pin.ck_idle_edge = 0;
    mReg.user.ck_out_edge = 0;
    mReg.ctrl2.miso_delay_mode = (freqDiv > 2) ? 2 : 0;
    mReg.ctrl2.miso_delay_num = 0;
    mReg.ctrl2.mosi_delay_mode = 0;
    mReg.ctrl2.mosi_delay_num = 0;
    // disable fast read modes
    mReg.ctrl.fread_qio = 0;
    mReg.ctrl.fread_dio = 0;
    mReg.ctrl.fread_quad = 0;
    mReg.ctrl.fread_dual = 0;
    // disable fast write modes
    mReg.user.fwrite_dio = 0;
    mReg.user.fwrite_dual = 0;
    mReg.user.fwrite_qio = 0;
    mReg.user.fwrite_quad = 0;

    // disable full duplex mode
    mReg.user.doutdin = 0;
    // enable 3-wire half duplex mode
    mReg.user.sio = 1;
    // whole fifo is for sending
    mReg.user.usr_mosi_highpart = 0;
    // disable all interrupts
    mReg.slave.val &= ~SPI_INT_EN_M;
    // clear transaction done flag
    mReg.slave.trans_done = 0;
    fifoMemset(0);
}
void SpiMaster::fifoMemset(uint32_t val, int nWords)
{
    if (nWords > 16) {
        ESP_LOGE("SPI", "Assertion failed: fifoMemset: nWords=%d > 16", nWords);
        return;
    }
    auto end = fifo() + nWords;
    for (auto word = fifo(); word < end; word++) {
        *word = val;
    }
}
void SpiMaster::configPins(const SpiPinCfg& pins)
{
/*
    gpio_set_direction((gpio_num_t)pins.mosi, GPIO_MODE_OUTPUT);
    gpio_iomux_in(pins.mosi, spi_periph_signal[mSpiHost].spid_in);
    gpio_iomux_out(pins.mosi, spi_periph_signal[mSpiHost].func, false);

    gpio_set_direction((gpio_num_t)pins.clk, GPIO_MODE_OUTPUT);
    gpio_iomux_in(pins.clk, spi_periph_signal[mSpiHost].spiclk_in);
    gpio_iomux_out(pins.clk, spi_periph_signal[mSpiHost].func, false);

    gpio_set_direction((gpio_num_t)pins.cs, GPIO_MODE_OUTPUT);
    gpio_iomux_in(pins.cs, spi_periph_signal[mSpiHost].spics_out[0]);
    gpio_iomux_out(pins.cs, spi_periph_signal[mSpiHost].func, false);
*/
    gpio_set_direction((gpio_num_t)pins.mosi, GPIO_MODE_INPUT_OUTPUT);
    gpio_matrix_out(pins.mosi, spi_periph_signal[mSpiHost].spid_out, false, false);
    gpio_matrix_in(pins.mosi, spi_periph_signal[mSpiHost].spid_in, false);

    gpio_set_direction((gpio_num_t)pins.clk, GPIO_MODE_INPUT_OUTPUT);
    gpio_matrix_out(pins.clk, spi_periph_signal[mSpiHost].spiclk_out, false, false);
    gpio_matrix_in(pins.clk, spi_periph_signal[mSpiHost].spiclk_in, false);

    gpio_set_direction((gpio_num_t)pins.cs, GPIO_MODE_INPUT_OUTPUT);
    gpio_matrix_out(pins.cs, spi_periph_signal[mSpiHost].spics_out[0], false, false);
    gpio_matrix_in(pins.cs, spi_periph_signal[mSpiHost].spics_in, false);

}

void SpiMaster::spiSend(const void* data, int size)
{
    while(size > 0) {
        waitDone();
        int count = std::min(size, 64);
        uint8_t* src = (uint8_t*)data;
        auto srcEnd = src + count;
        volatile uint32_t* dest = fifo();
        volatile uint32_t* destEnd = dest + count / 4;
        for(; dest < destEnd; src += 4) {
            *dest++ = *((uint32_t*)src);
        }
        if (src < srcEnd) {
            uint32_t val = *src++;
            if (src < srcEnd) {
                val |= (*src++) << 8;
                if (src < srcEnd) {
                    val |= (*src++) << 16;
                }
            }
            *dest = val;
        }
        size -= count;
        fifoSend(count);
    }
}
