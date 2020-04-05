#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <audio_element.h>
#include <http_stream.h>
#include <i2s_stream.h>
#include <esp_log.h>
#include <memory>

template<typename T>
struct BufPtr
{
protected:
    T* mPtr;
    BufPtr() {} // mPtr remains uninitialized, only for derived classes
public:
    T* ptr() { return mPtr; }
    BufPtr(T* ptr): mPtr(ptr){}
    BufPtr(BufPtr<T>&& other) {
        mPtr = other.mPtr;
        other.mPtr = nullptr;
    }
    ~BufPtr() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void free() {
        if (mPtr) {
            ::free(mPtr);
        }
    }
    void freeAndReset(T* newPtr) {
        free();
        mPtr = newPtr;
    }
    void* release() {
        auto ret = mPtr;
        mPtr = nullptr;
        return ret;
    }
};
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

bool unescapeUrlParam(char* str, size_t len);

uint8_t hexDigitVal(char digit);
class UrlParams: public BufPtr<char>
{
public:
    struct Substring
    {
        const char* str;
        size_t len;
        Substring(const char* aStr, size_t aLen): str(aStr), len(aLen) {}
        Substring() {}
        operator bool() const { return str != nullptr; }
    };
    struct KeyVal
    {
        Substring key;
        Substring val;
    };
protected:
    size_t mSize;
    std::vector<KeyVal> mKeyVals;
    bool parse();
public:
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    UrlParams(httpd_req_t* req);
    Substring strParam(const char* name);
    long intParam(const char* name, long defVal);
};
const char* getUrlFile(const char* url);

static inline TaskHandle_t currentTaskHandle()
{
    extern volatile void * volatile pxCurrentTCB;
    return (TaskHandle_t)pxCurrentTCB;
}

class Mutex
{
    SemaphoreHandle_t mMutex;
    StaticSemaphore_t mMutexMem;
public:
    Mutex() {
        mMutex = xSemaphoreCreateMutexStatic(&mMutexMem);
    }
    void lock() { xSemaphoreTake(mMutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mMutex); }
};

class MutexLocker
{
    Mutex& mMutex;
public:
    MutexLocker(Mutex& aMutex): mMutex(aMutex) { mMutex.lock(); }
    ~MutexLocker() { mMutex.unlock(); }
};

namespace std {
template<>
    struct default_delete<FILE>
    {
        void operator()(FILE* file) const { fclose(file); }
    };
}
extern "C" const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
extern "C" const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT;

#endif
