#ifndef NETPLAYER_EXCEPTION_H
#define NETPLAYER_EXCEPTION_H
#include <exception>

class MyException: public std::exception
{
protected:
    char* mMessage;
    int mCode;
public:
    MyException(const char* msg, int code=0)
    : mMessage(msg ? strdup(msg) : nullptr), mCode(code)
    {}
    ~MyException() { free(mMessage); }
    const char* what() const noexcept override { return mMessage; }
    int code() const { return mCode; }
    MyException& operator=(const MyException& other)
    {
        mCode = other.mCode;
        free(mMessage);
        mMessage = strdup(other.mMessage);
        return *this;
    }
};
#endif
