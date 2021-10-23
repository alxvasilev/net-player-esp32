#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <memory>
#include <string>
#include "buffer.hpp"
#include "mutex.hpp"
#include "timer.hpp"

#define myassert(cond) if (!(cond)) { \
    ESP_LOGE("ASSERT", "Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__); \
    *((int*)nullptr) = 0; }

#define TRACE ESP_LOGI("TRC", "%s:%d", __FILE__, __LINE__);

#define MY_STRINGIFY_HELPER(x) #x
#define MY_STRINGIFY(x) MY_STRINGIFY_HELPER(x)

char* binToHex(const uint8_t* data, size_t len, char* str);

extern const char* _utils_hexDigits;
template <typename T>
char* numToHex(T val, char* str)
{
    const uint8_t* start = (const uint8_t*) &val;
    const uint8_t* data = start + sizeof(T) - 1;
    while (data >= start) {
        *(str++) = _utils_hexDigits[*data >> 4];
        *(str++) = _utils_hexDigits[*data & 0x0f];
        data--;
    }
    *str = 0;
    return str;
}
std::string binToAscii(char* buf, int len, int lineLen=32);
bool unescapeUrlParam(char* str, size_t len);

uint8_t hexDigitVal(char digit);
long strToInt(const char* str, size_t len, long defVal, int base=10);
float strToFloat(const char* str, size_t len, float defVal);
class KeyValParser
{
public:
    struct Substring
    {
        char* str;
        size_t len;
        Substring(char* aStr, size_t aLen): str(aStr), len(aLen) {}
        Substring() {}
        operator bool() const { return str != nullptr; }
        void trimSpaces();
        long toInt(long defVal, int base=10) const { return strToInt(str, len, defVal, base); }
        float toFloat(float defVal) const { return strToFloat(str, len, defVal); }
    };
    struct KeyVal
    {
        Substring key;
        Substring val;
    };
protected:
    char* mBuf;
    size_t mSize;
    std::vector<KeyVal> mKeyVals;
    bool mOwn;
    KeyValParser() {} // ctor to inherit when derived class has its own initialization
public:
    enum Flags: uint8_t { kUrlUnescape = 1, kTrimSpaces = 2 };
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    KeyValParser(char* str, size_t len, bool own=false): mBuf(str), mSize(len), mOwn(own) {}
    ~KeyValParser();
    bool parse(char pairDelim, char keyValDelim, Flags flags);
    Substring strVal(const char* name);
    long intVal(const char* name, long defVal);
    float floatVal(const char* name, float defVal);
};

class UrlParams: public KeyValParser
{
public:
    UrlParams(httpd_req_t* req);
};
std::string jsonStringEscape(const char* str);

const char* getUrlFile(const char* url);
class FileHandle: public std::unique_ptr<FILE, void(*)(FILE*)>
{
protected:
    typedef std::unique_ptr<FILE, void(*)(FILE*)> Base;
public:
    FileHandle(FILE* f): Base(f, [](FILE* fp) { if (fp) fclose(fp); }) {}
};

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
}

static inline void usDelay(uint32_t us)
{
    auto end = esp_timer_get_time() + us;
    while (esp_timer_get_time() < end);
}

static inline void msDelay(uint32_t ms)
{
    usDelay(ms * 1000);
}

struct AsyncMsgBase
{
    esp_timer_handle_t mTimer = nullptr;
    void post()
    {
        ESP_ERROR_CHECK(esp_timer_start_once(mTimer, 0));
    }
    ~AsyncMsgBase() {
        if (mTimer) {
            esp_timer_stop(mTimer);
            esp_timer_delete(mTimer);
        }
    }
};

template <class Cb>
static void asyncCall(Cb&& func)
{
    struct AsyncMsg: public AsyncMsgBase {
        Cb mCallback;
        AsyncMsg(Cb&& cb): mCallback(std::forward<Cb>(cb))
        {
            esp_timer_create_args_t args = {};
            args.dispatch_method = ESP_TIMER_TASK;
            args.name = "setTimeout timer";
            args.arg = this;
            args.callback = &onTimer;
            ESP_ERROR_CHECK(esp_timer_create(&args, &mTimer));
        }
        static void onTimer(void* ctx) {
            auto self = static_cast<AsyncMsg*>(ctx);
            try {
                self->mCallback();
            } catch(std::exception& e) {
            }
            delete self;
        }
    };
    auto msg = new AsyncMsg(std::forward<Cb>(func));
    msg->post();
}

class ElapsedTimer
{
protected:
    int64_t mTsStart;
public:
    ElapsedTimer(): mTsStart(esp_timer_get_time()) {}
    int msStartTime() const { return (mTsStart + 500 ) / 1000; }
    int64_t usStartTime() const { return mTsStart; }
    int64_t usElapsed() const { return esp_timer_get_time() - mTsStart; }
    int msElapsed() const { return (usElapsed() + 500) / 1000; }
    void reset() { mTsStart = esp_timer_get_time(); }
};

namespace std {
template<>
    struct default_delete<FILE>
    {
        void operator()(FILE* file) const { fclose(file); }
    };
}

int16_t currentCpuFreq();

#endif
