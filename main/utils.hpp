#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <memory>
#include "buffer.hpp"
#include "mutex.hpp"
#include "timer.hpp"

#define myassert(cond) if (!(cond)) { \
    ESP_LOGE("ASSERT", "Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__); \
    *((int*)nullptr) = 0; }

#define TRACE ESP_LOGI("TRC", "%s:%d", __FILE__, __LINE__);

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

class KeyValParser: public BufPtr<char>
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
    };
    struct KeyVal
    {
        Substring key;
        Substring val;
    };
protected:
    size_t mSize;
    std::vector<KeyVal> mKeyVals;
    KeyValParser() {} // ctor to inherit when derived class has its own initialization
public:
    enum Flags: uint8_t { kUrlUnescape = 1, kTrimSpaces = 2 };
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    KeyValParser(char* str, size_t len): BufPtr(str), mSize(len) {}
    bool parse(char pairDelim, char keyValDelim, Flags flags);
    Substring strVal(const char* name);
    long intVal(const char* name, long defVal);
};

class UrlParams: public KeyValParser
{
public:
    UrlParams(httpd_req_t* req);
    Substring strParam(const char* name) { return strVal(name); }
    long intParam(const char* name, long defVal) { return intVal(name, defVal); }
};

const char* getUrlFile(const char* url);

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
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
