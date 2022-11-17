#ifndef NETPLAYER_UTILS_PARSE_HPP
#define NETPLAYER_UTILS_PARSE_HPP

#include <vector>
#include <stdarg.h>
#include <memory>
#include <string>
//#include "buffer.hpp"

char* binToHex(const uint8_t* data, size_t len, char* str, char delim=' ');

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
        // Does not include the terminating null
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
    enum Flags: uint8_t { kUrlUnescape = 1, kTrimSpaces = 2, kKeysToLower = 4 };
    const std::vector<KeyVal>& keyVals() const { return mKeyVals; }
    std::vector<KeyVal>& keyVals() { return mKeyVals; }
    /** Creates the parser, no parsing is peformed yet.
     *  \param len must include the terminating null
     */
    KeyValParser(char* str, size_t len, bool own=false): mBuf(str), mSize(len), mOwn(own) {}
    ~KeyValParser();
    /** Destructively parse the buffer to key-value pairs, collected in the keyVals() vector.
     *  Each Substring is null-terminated, and the length does not include the null terminator
     */
    bool parse(char pairDelim, char keyValDelim, int flags);
    Substring strVal(const char* name);
    long intVal(const char* name, long defVal);
    float floatVal(const char* name, float defVal);
};

std::string jsonStringEscape(const char* str);
const char* getUrlFile(const char* url);

#endif
