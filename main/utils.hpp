#ifndef NETPLAYER_UTILS_HPP
#define NETPLAYER_UTILS_HPP

#include <esp_http_server.h>
#include <vector>

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
uint8_t hexDigitVal(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    } else if (digit >= 'a' && digit <= 'f') {
        return 10 + (digit - 'a');
    } else if (digit >= 'A' && digit <= 'F') {
        return 10 + (digit - 'A');
    } else {
        return 0xff;
    }
}
void binToHex(const uint8_t* data, size_t len, char* str) {
    static const char* digits = "01234567890abcdef";
    auto end = data + len;
    while (data < end) {
        *(str++) = digits[*data >> 4];
        *(str++) = digits[*data & 0x0f];
        data++;
    }
    *str = 0;
}

bool unescapeUrlParam(char* str, size_t len)
{
    const char* rptr = str;
    char* wptr = str;
    const char* end = str + len;
    bool ok = true;
    for (; rptr < end; rptr++, wptr++) {
        char ch = *rptr;
        if (ch != '%') {
            if (rptr != wptr) {
                *wptr = ch;
            }
        } else {
            rptr++;
            auto highNibble = hexDigitVal(*(rptr++));
            auto lowNibble = hexDigitVal(*rptr);
            if (highNibble > 15 || lowNibble > 15) {
                *wptr = '?';
                ok = false;
            }
            *wptr = (highNibble << 4) | lowNibble;
        }
    }
    if (wptr < rptr) {
        *wptr = 0;
    }
    return ok;
}
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

extern "C" const i2s_stream_cfg_t myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
extern "C" const http_stream_cfg_t myHTTP_STREAM_CFG_DEFAULT;

#endif
