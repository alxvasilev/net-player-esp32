#include "utils.hpp"
#include <soc/rtc.h>

const char* _utils_hexDigits = "0123456789abcdef";

char* binToHex(const uint8_t* data, size_t len, char* str)
{
    auto end = data + len;
    while (data < end) {
        *(str++) = _utils_hexDigits[*data >> 4];
        *(str++) = _utils_hexDigits[*data & 0x0f];
        data++;
    }
    *str = 0;
    return str;
}

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

bool UrlParams::parse()
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
UrlParams::UrlParams(httpd_req_t* req)
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

UrlParams::Substring UrlParams::strParam(const char* name)
{
    for (auto& keyval: mKeyVals) {
        if (strcmp(name, keyval.key.str) == 0) {
            return keyval.val;
        }
    }
    return Substring(nullptr, 0);
}
long UrlParams::intParam(const char* name, long defVal)
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

const char* getUrlFile(const char* url)
{
    const char* lastSlashPos = nullptr;
    for (;;url++) {
        if (*url == 0) {
            break;
        } else if (*url == '/') {
            lastSlashPos = url;
        }
    }
    return lastSlashPos + 1;
}

int16_t currentCpuFreq() {
    rtc_cpu_freq_config_t conf;
    rtc_clk_cpu_freq_get_config(&conf);
    return conf.freq_mhz;
}
