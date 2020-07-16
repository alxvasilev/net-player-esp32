#include "spdif.hpp"
#include <soc/rmt_struct.h>
#include <soc/periph_defs.h>
#include <soc/io_mux_reg.h>
#include <driver/periph_ctrl.h>
#include <hal/rmt_ll.h>
#include <hal/rmt_hal.h>
#include <driver/pcnt.h>
#include <esp_timer.h>
#include "utils.hpp"

void gpioOutTask(void* arg) {
    const gpio_num_t pin = GPIO_NUM_2;
    gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    for (;;) {
        for(int i = 0; i < 10; i++) {
            gpio_set_level(pin, 1);
            //for (int i= 0; i<10; i++);
            gpio_set_level(pin, 0);
            //for (int i= 0; i<100; i++);
        }
        usDelay(10);
    }
}
//#define TEST

RmtRx::RmtRx(gpio_num_t inputPin)
: mChan0(0), mInputPin(inputPin), mChan1(4)
{
    pcntInit(); // initializes the inputPin as GPIO input with pullup
//    rmt_module_enable();
    periph_module_reset(PERIPH_RMT_MODULE);
    periph_module_enable(PERIPH_RMT_MODULE);
//    rmt_ll_enable_mem_access(&RMT, true);
    RMT.apb_conf.fifo_mask = true; // disable fifo access to RMT memory, use RMTMEM access
    mChan0.init(inputPin, false);
    mChan1.init(inputPin, false);
    xTaskCreate(&gpioOutTask, "testGpio", 2048, nullptr, 4, nullptr);
}

void RmtRx::pcntInit()
{
    pcnt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    // Set PCNT input signal and control GPIOs
    cfg.pulse_gpio_num = mInputPin;
    cfg.ctrl_gpio_num = -1;
    cfg.unit = PCNT_UNIT_0;
    cfg.channel = PCNT_CHANNEL_0;
    // What to do on the positive / negative edge of pulse input?
    cfg.pos_mode = PCNT_COUNT_INC;   // Count up on the positive edge
    cfg.neg_mode = PCNT_COUNT_INC;   // Keep the counter value on the negative edge
    // What to do when control input is low or high?
    cfg.lctrl_mode = PCNT_MODE_KEEP; // Reverse counting direction if low
    cfg.hctrl_mode = PCNT_MODE_KEEP;    // Keep the primary counter mode if high
        // Set the maximum and minimum limit values to watch
    cfg.counter_h_lim = 0x7fff;
    cfg.counter_l_lim = 0;
    /* Initialize PCNT unit */
    pcnt_unit_config(&cfg);
}

void RmtRx::start() {
    assert(mActiveChanNo < 0);
    mActiveChanNo = 0;
    mIsFirstBatch = true;
    mChan0.setMemOwner(RMT_MEM_OWNER_SW);
    mChan0.zeroFifo();
    pcnt_counter_clear(PCNT_UNIT_0);
    pcnt_counter_resume(PCNT_UNIT_0);
    mChan0.start();
}

int16_t RmtRx::pcntCurrPulseCount()
{
    int16_t pulseCnt;
    pcnt_get_counter_value(PCNT_UNIT_0, &pulseCnt);
    return pulseCnt;
}

void RmtRx::pcntReset()
{
    pcnt_counter_clear(PCNT_UNIT_0);
}

volatile rmt_item32_t* RmtRx::getPulses()
{
    assert(mActiveChanNo >= 0);

    RmtRxChannel* active;
    RmtRxChannel* other;
    if (mActiveChanNo == 0) {
        active = &mChan0;
        other = &mChan1;
    } else {
        active = &mChan1;
        other = &mChan0;
    }
    other->zeroFifo();

    // wait for at least this amount of pulses
    if (pcntCurrPulseCount() > kMaxPulsesPerBatch) {
        ESP_LOGI("RMT", "Too late with %d pulses", pcntCurrPulseCount() - kMaxPulsesPerBatch);
    }
    while(pcntCurrPulseCount() < kMaxPulsesPerBatch);

    // stop on capture transition from 1 to 0
    while(gpio_get_level(mInputPin) != 1);
    while(gpio_get_level(mInputPin) != 0);

    other->start();
    active->stop();
    pcntReset();

    active->setMemOwner(false);
    mActiveChanNo = other->chanNo();
    return active->memory();
}

void RmtRxChannel::init(gpio_num_t pin, bool initPinGpio)
{
//    rmt_set_pin(ChanNo, RMT_MODE_RX, pin);
    if (initPinGpio) {
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
        gpio_set_direction(pin, GPIO_MODE_INPUT);
    }
    gpio_matrix_in(pin, RMT_SIG_IN0_IDX + mChanNo, 0);

    clearInterrupts();
    // below code is similar to rmt_internal_config

//    rmt_ll_set_counter_clock_div(&RMT, ChanNo, 1);
    mConf0Reg.div_cnt = 1;
//    rmt_ll_reset_rx_pointer(&RMT, channel);
    resetRxPointer();
// clock src: APB_CLK
//    rmt_ll_set_counter_clock_src(dev, channel, RMT_BASECLK_APB);
    mConf1Reg.ref_always_on = RMT_BASECLK_APB;
//    rmt_ll_set_mem_blocks(dev, channel, mem_cnt);
    mConf0Reg.mem_size = 4;
//    rmt_ll_set_mem_owner(dev, channel, RMT_MEM_OWNER_HW);
    setMemOwner(RMT_MEM_OWNER_HW);

    /*Set idle threshold*/
//    rmt_ll_set_rx_idle_thres(dev, channel, threshold);
    mConf0Reg.idle_thres = 0xffff;
    /* Set RX filter */
//    rmt_ll_set_rx_filter_thres(dev, channel, filter_cnt);
    mConf1Reg.rx_filter_thres = 1;
//    rmt_ll_enable_rx_filter(dev, channel, rmt_param->rx_config.filter_en);
    mConf1Reg.rx_filter_en = false;
}

void RmtRxChannel::clearInterrupts()
{
    rmt_ll_enable_err_interrupt(&RMT, mChanNo, false);
    rmt_ll_enable_tx_end_interrupt(&RMT, mChanNo, false);
    rmt_ll_enable_tx_thres_interrupt(&RMT, mChanNo, false);
    rmt_ll_enable_rx_end_interrupt(&RMT, mChanNo, false);
    rmt_ll_clear_err_interrupt(&RMT, mChanNo);
    rmt_ll_clear_tx_end_interrupt(&RMT, mChanNo);
    rmt_ll_clear_tx_thres_interrupt(&RMT, mChanNo);
    rmt_ll_clear_rx_end_interrupt(&RMT, mChanNo);
}

SpdifInputNode::SpdifInputNode(gpio_num_t inputPin)
:AudioNodeWithTask("node-spdif-in", 4096, 22), mRmtRx(inputPin)
{
    ESP_LOGW("SPDIF", "Creating SPDIF node");
}

void SpdifInputNode::nodeThreadFunc()
{
    ESP_LOGW("SPDIF", "Started SPDIF node");
    rmt_item32_t pulses1[10];
    rmt_item32_t pulses2[10];
    mRmtRx.start();
    for (;;) {
        auto pulses = mRmtRx.getPulses();

        memcpy((void*)&pulses1, (uint8_t*)pulses+1024-sizeof(pulses1), sizeof(pulses1));
        pulses = mRmtRx.getPulses();
        memcpy(&pulses2, (void*)pulses, sizeof(pulses2));

        ESP_LOGI("", "Batch0");
        for (int i = 0, p = 0; i<10; i++) {
            ESP_LOGI("", "[%d] %d: %d", p++, pulses1[i].level0, pulses1[i].duration0);
            ESP_LOGI("", "[%d] %d: %d", p++, pulses1[i].level1, pulses1[i].duration1);
        }
        ESP_LOGI("", "Batch1");
        for (int i = 0, p = 0; i<10; i++) {
            ESP_LOGI("", "[%d] %d: %d", p++, pulses2[i].level0, pulses2[i].duration0);
            ESP_LOGI("", "[%d] %d: %d", p++, pulses2[i].level1, pulses2[i].duration1);
        }
    }
    /*
    rmt_rx_start(mRxChan, true);

    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
#ifdef TEST
        for(;;) {
           ESP_LOGI(" ", "level: %d", gpio_get_level(GPIO_NUM_35));
        }
#endif
        RingbufHandle_t rb;
        rmt_get_ringbuf_handle(mRxChan, &rb);
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            size_t count;
            auto items = (rmt_item32_t*)xRingbufferReceive(rb, &count, 1000 / portTICK_PERIOD_MS);
            if (!items) { // timeout
                ESP_LOGI("SPDIF", "Recv timeout");
                continue;
            }
            ESP_LOGI("SPIDF", "received items %p (size = %d bytes)",items, count);
            count >>= 2; // one RMT = 4 Bytes
            parseSpdifPulses(items, count);
            //after parsing the data, return spaces to ringbuffer.
            vRingbufferReturnItem(rb, (void *) items);
        }
//        rmt_rx_stop(mRxChan);
    }
    */
}

void SpdifInputNode::parseSpdifPulses(rmt_item32_t* data, size_t count)
{
    if (count > 64) {
        count = 64;
    }
    for (int i = 0; i<count; i++) {
        auto item = data[i];
        ESP_LOGI(" ", "%d : %d", item.level0, item.duration0);
        ESP_LOGI(" ", "%d : %d", item.level1, item.duration1);
    }
}
