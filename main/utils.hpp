#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>
#include <stdarg.h>
#include <audio_element.h>
#include <http_stream.h>
#include <i2s_stream.h>
#include <esp_log.h>

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
void binToHex(const uint8_t* data, size_t len, char* str);
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
    bool parse()
    {
        auto end = mPtr + mSize - 1;
        char* pch = mPtr;
        for (;;) {
            auto start = pch;
            for (; (pch < end) && (*pch != '='); pch++);
            if (pch >= end) { // unexpected end
                return false;
            }
            *pch = 0; // null-terminate the key
            mKeyVals.emplace_back();
            KeyVal& keyval = mKeyVals.back();
            auto& key = keyval.key;
            key.str = start;
            key.len = pch - start;

            start = ++pch;
            for (; (pch < end) && (*pch != '&'); pch++);
            auto& val = keyval.val;
            auto len = pch - start;
            if (!unescapeUrlParam(start, len)) {
                return false;
            }
            val.str = start;
            val.len = len;
            if (pch >= end) {
                assert(pch == end);
                assert(*pch == 0);
                return true;
            }
            *(pch++) = 0; // null-terminate the value
        }
    }
public:
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    UrlParams(httpd_req_t* req)
    {
        mSize = httpd_req_get_url_query_len(req) + 1;
        if (mSize <= 1) {
            mSize = 0;
            mPtr = nullptr;
            return;
        }
        mPtr = (char*)malloc(mSize);
        if (httpd_req_get_url_query_str(req, mPtr, mSize) != ESP_OK) {
            free();
            return;
        }
        parse();
    }
    Substring strParam(const char* name)
    {
        for (auto& keyval: mKeyVals) {
            if (strcmp(name, keyval.key.str) == 0) {
                return keyval.val;
            }
        }
        return Substring(nullptr, 0);
    }
    long intParam(const char* name, long defVal)
    {
        auto strVal = strParam(name);
        auto str = strVal.str;
        if (!str) {
            return defVal;
        }
        char* endptr;
        auto val = strtol(str, &endptr, 10);
        return (endptr == str + strVal.len) ? val : defVal;
    }
};

class StdoutRedirector
{
protected:
    FILE* mOrigStdout = nullptr;
    FILE* mNewStdout = nullptr;
    bool mDisableDefault;
    static StdoutRedirector* gInstance;
    void(*mOutputFunc)(const char* data, int len) = nullptr;
    static void defaultOutputFunc(const char* data, int len)
    {
        // just write to stdout
        fwrite(data, 1, len, gInstance->mOrigStdout);
    }
    static int espVprintf(const char * format, va_list args)
    {
        return vfprintf(gInstance->mNewStdout, format, args);
    }
    static int logFdWriteFunc(void* cookie, const char* data, int size)
    {
        if (!gInstance->mDisableDefault) {
            defaultOutputFunc(data, size);
        }
        if (gInstance->mOutputFunc) {
            gInstance->mOutputFunc(data, size);
        }
        return size;
    }
public:
    StdoutRedirector(bool disableDefault)
    {
        if (gInstance) {
            abort();
        }
        gInstance = this;
        mDisableDefault = disableDefault;
        mOrigStdout = stdout;
        mNewStdout = stdout = funopen(nullptr, nullptr, &logFdWriteFunc, nullptr, nullptr);
        esp_log_set_vprintf(&espVprintf);
    }
};

extern "C" const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
extern "C" const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT;

#endif
