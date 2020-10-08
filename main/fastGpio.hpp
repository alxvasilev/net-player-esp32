#ifndef FAST_GPIO
#define FAST_GPIO

#include <type_traits>

template <gpio_num_t Pin>
struct GpioPinStaticBase
{
    static void configAsOutput() {
        gpio_pad_select_gpio(Pin);
        gpio_set_direction(Pin, GPIO_MODE_OUTPUT);
    }
};

template <gpio_num_t Pin, typename=void>
class GpioPinStatic;

template<gpio_num_t Pin>
class GpioPinStatic<Pin, typename std::enable_if<(Pin < 32)>::type>: public GpioPinStaticBase<Pin>
{
public:
    static uint32_t read() { return (GPIO.in & (1 << Pin)); }
    static void set() { GPIO.out_w1ts = (1 << Pin); }
    static void clear() { GPIO.out_w1tc = (1 << Pin); }
};

template<gpio_num_t Pin>
class GpioPinStatic<Pin, typename std::enable_if<(Pin >= 32)>::type>: public GpioPinStaticBase<Pin>
{
public:
    static void configAsOutput() {
        gpio_pad_select_gpio(Pin);
        gpio_set_direction(Pin, GPIO_MODE_OUTPUT);
    }
    static uint32_t read() { return (GPIO.in1.data & (1 << (Pin - 32))); }
    static void set() { GPIO.out1_w1ts.data = (1 << (Pin - 32)); }
    static void clear() { GPIO.out1_w1tc.data = (1 << (Pin - 32)); }
};


class GpioPin
{
protected:
    uint32_t mMask;
    volatile uint32_t* mReadReg;
    volatile uint32_t* mSetReg;
    volatile uint32_t* mClrReg;
    uint8_t mPinNum;
public:
    GpioPin(uint8_t pin): mPinNum(pin) {
        if (pin < 32) {
            mMask = (1 << pin);
            mReadReg = &(GPIO.in);
            mSetReg = &(GPIO.out_w1ts);
            mClrReg = &(GPIO.out_w1tc);
        } else {
            mMask = (1 << (pin - 32));
            mReadReg = (volatile uint32_t*)&(GPIO.in1);
            mSetReg = (volatile uint32_t*)&(GPIO.out1_w1ts);
            mClrReg = (volatile uint32_t*)&(GPIO.out1_w1tc);
        }
    }
    gpio_num_t pinNum() const { return (gpio_num_t)mPinNum; }
    uint32_t read() const { return (*mReadReg) & mMask; }
    void set() { *mSetReg = mMask; }
    void clear() { *mClrReg = mMask; }
};

#endif
