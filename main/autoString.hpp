#ifndef AUTO_STRING_HPP
#define AUTO_STRING_HPP

class AutoCString
{
protected:
    const char* mStr;
public:
    const char* c_str() const { return mStr; }
    AutoCString(const char* str): mStr(str){}
    ~AutoCString() { clear(); }
    void clear() { if (mStr) free((void*)mStr); }
    operator bool() const { return mStr != nullptr; }
};
#endif
