#ifndef BUFFER_HPP_INCLUDED
#define BUFFER_HPP_INCLUDED

#include <stdarg.h>
#include <esp_log.h>
#include <memory>
#include <stdlib.h>

template<typename T>
struct BufPtr
{
protected:
    T* mPtr;
    BufPtr() {} // mPtr remains uninitialized, only for derived classes
public:
    T* ptr() { return mPtr; }
    const T* ptr() const { return mPtr; }
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
        if (!mPtr) {
            return;
        }
        ::free(mPtr);
        mPtr = nullptr;
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
    operator bool() const { return mPtr != nullptr; }
};

class DynBuffer
{
protected:
    char* mBuf;
    int mBufSize;
    int mDataSize;
public:
    char* buf() { return mBuf; }
    const char* buf() const { return mBuf; }
    int capacity() const { return mBufSize; }
    int dataSize() const { return mDataSize; }
    bool isEmpty() const { return mDataSize <= 0; }
    int freeSpace() const { return mBufSize - mDataSize; }
    DynBuffer(size_t allocSize = 0)
    :mDataSize(0)
    {
        if (!allocSize) {
            mBuf = nullptr;
            mBufSize = 0;
            return;
        }
        mBuf = (char*)malloc(allocSize);
        if (!mBuf) {
            mBufSize = 0;
            return;
        }
        mBufSize = allocSize;
    }
    DynBuffer(char* data, size_t size) {
        if (!data || !size) {
            mBuf = nullptr;
            mBufSize = mDataSize = 0;
            return;
        }
        mBuf = (char*)malloc(size);
        assert(mBuf);
        memcpy(mBuf, data, size);
        mBufSize = mDataSize = size;
    }
    ~DynBuffer() { if (mBuf) ::free(mBuf); }
    void clear() { mDataSize = 0; }
    void freeBuf()
    {
        if (!mBuf) {
            return;
        }
        ::free(mBuf);
        mBuf = nullptr;
        mBufSize = mDataSize = 0;
    }
    char& operator[](int idx)
    {
        assert(idx >= 0 && idx < mDataSize);
        return mBuf[idx];
    }
    operator bool() const { return mBuf && mDataSize > 0; }
    void reserve(int newSize)
    {
        if (newSize <= mBufSize) {
            return;
        }
        auto newBuf = mBuf ? (char*)realloc(mBuf, newSize) : (char*)malloc(newSize);
        if (!newBuf) {
            ESP_LOGE("BUF", "reserve: Out of memory allocating %d bytes for buffer", newSize);
            return;
        }
        mBuf = newBuf;
        mBufSize = newSize;
    }
    void resize(int newDataSize)
    {
        if (newDataSize > mBufSize) {
            reserve(newDataSize);
        }
        mDataSize = newDataSize;
    }
    void ensureFreeSpace(int amount)
    {
        auto needed = mDataSize + amount;
        if (needed > mBufSize) {
            reserve(needed);
        }
    }
    char* getAppendPtr(int writeLen)
    {
        ensureFreeSpace(writeLen);
        return mBuf + mDataSize;
    }
    void expandDataSize(int by)
    {
        auto newDataSize = mDataSize + by;
        assert(newDataSize <= mBufSize);
        mDataSize = newDataSize;
    }
    void setDataSize(int newSize)
    {
        mDataSize = (newSize > mBufSize) ? mBufSize : newSize;
    }
    void assign(const char* data, int size) {
        resize(size);
        memcpy(mBuf, data, size);
        mDataSize = size;
    }
    void moveFrom(DynBuffer& other) {
        if (mBuf) {
            ::free(mBuf);
        }
        mBufSize = other.mBufSize;
        mDataSize = other.mDataSize;
        mBuf = other.release();
    }
    char* release() {
        auto result = mBuf;
        mBuf = nullptr;
        mBufSize = mDataSize = 0;
        return result;
    }
    DynBuffer& append(const char* data, int dataSize)
    {
        ensureFreeSpace(dataSize);
        memcpy(mBuf + mDataSize, data, dataSize);
        mDataSize += dataSize;
        return *this;
    }
    template<class T>
    DynBuffer& append(T val) { return append((const char*)&val, sizeof(val)); }
    DynBuffer& appendChar(char ch)
    {
        ensureFreeSpace(1);
        mBuf[mDataSize++] = ch;
        return *this;
    }
    DynBuffer& appendStr(const char* str, bool nullTerminate=false)
    {
        auto len = strlen(str);
        if (len) {
            append(str, nullTerminate ? len+1 : len);
        } else if (nullTerminate) {
            appendChar(0);
        }
        return *this;
    }
    void nullTerminate()
    {
        if (!mDataSize || mBuf[mDataSize-1] != 0) {
            appendChar(0);
        }
    }
    void truncateChar(int num=1)
    {
        int newDataSize = mDataSize - num;
        mDataSize = (newDataSize < 0) ? 0 : newDataSize;
    }
    int vprintf(const char *fmt, va_list args)
    {
        if ((mDataSize > 0) && (mBuf[mDataSize - 1] == 0)) {
            mDataSize--;
        }
        int writeSize = freeSpace();
        for (;;) {
            int num = ::vsnprintf(getAppendPtr(writeSize), writeSize, fmt, args);
            if (num < 0) {
                return num;
            } else if (num < writeSize) { // completely written
                expandDataSize(num + 1);
                return num;
            } else {
                writeSize = num + 1;
            }
        }
    }
    int printf(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        int ret = vprintf(fmt, args);
        va_end(args);
        return ret;
    }
    const char* toString()
    {
        if (!dataSize()) {
            return nullptr;
        }
        DynBuffer result(dataSize());
        auto end = mBuf + dataSize();
        for (char* ptr = mBuf; ptr < end; ptr++) {
            char ch = *ptr;
            if (ch >= 32 && ch < 127 && ch != '\\') {
                result.appendChar(ch);
            } else {
                result.appendChar('\\');
                char buf[5];
                result.appendStr(itoa(ch, buf, 16));
            }
        }
        result.nullTerminate();
        return result.release();
    }
};

#endif
