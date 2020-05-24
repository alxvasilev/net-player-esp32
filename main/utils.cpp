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
std::string binToAscii(char* buf, int len, int lineLen)
{
    std::string result;
    result.reserve(len);
    for (int i = 0; i < len; i++) {
        if (i % lineLen == 0) {
            result += '\n';
            char buf[12];
            result.append(itoa(i, buf, 10));
            result += '\t';
            result += ':';
        }
        char ch = buf[i];
        if (ch >= 32 && ch <= 126) {
            result += ch;
        } else {
            result += '.';
        }
    }
    return result;
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

long strToInt(const char* str, size_t len, long defVal, int base)
{
    char* end;
    long val = strtol(str, &end, base);
    return (end == str + len) ? val : defVal;
}

void KeyValParser::Substring::trimSpaces()
{
    auto end = str + len;
    while (str < end && (*str == ' ' || *str == '\t')) {
        str++;
    }
    if (str == end) {
        str = nullptr;
        len = 0;
        return;
    }
    auto newEnd = end - 1;
    while (newEnd > str && (*newEnd == ' ' || *newEnd == '\t')) {
        newEnd--;
    }
    newEnd++;
    *newEnd = 0;
    len = newEnd - str;
}

bool KeyValParser::parse(char pairDelim, char keyValDelim, Flags flags)
{
    auto end = mPtr + mSize - 1;
    char* pch = mPtr;
    for (;;) {
        auto start = pch;
        for (; (pch < end) && (*pch != keyValDelim); pch++);
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
        for (; (pch < end) && (*pch != pairDelim); pch++);
        auto& val = keyval.val;
        auto len = pch - start;
        if (flags & kUrlUnescape) {
            if (!unescapeUrlParam(start, len)) {
                return false;
            }
        }
        val.str = start;
        val.len = len;
        if (pch >= end) {
            assert(pch == end);
            assert(*pch == 0);
            return true;
        }
        *(pch++) = 0; // null-terminate the value
        if (flags & kTrimSpaces) {
            keyval.key.trimSpaces();
            keyval.val.trimSpaces();
        }
    }
}

KeyValParser::Substring KeyValParser::strVal(const char* name)
{
    for (auto& keyval: mKeyVals) {
        if (strcmp(name, keyval.key.str) == 0) {
            return keyval.val;
        }
    }
    return Substring(nullptr, 0);
}
long KeyValParser::intVal(const char* name, long defVal)
{
    auto sval = strVal(name);
    auto str = sval.str;
    if (!str) {
        return defVal;
    }
    return sval.toInt(defVal);
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
    parse('&', '=', kUrlUnescape);
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


