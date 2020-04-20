#ifndef RINGBUF_HPP
#define RINGBUF_HPP

#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "utils.hpp"
#define rbassert myassert

struct EventGroup
{
    StaticEventGroup_t mEventStruct;
    EventGroupHandle_t mEventGroup;
    EventGroup(): mEventGroup(xEventGroupCreateStatic(&mEventStruct)) {}
    EventBits_t get() const { return xEventGroupGetBits(mEventGroup); }
    void setBits(EventBits_t bit) { xEventGroupSetBits(mEventGroup, bit); }
    void clearBits(EventBits_t bit) { xEventGroupClearBits(mEventGroup, bit); }

    EventBits_t wait(EventBits_t waitBits, BaseType_t all, BaseType_t autoReset, int msTimeout)
    {
        auto ret = xEventGroupWaitBits(mEventGroup, waitBits, autoReset, all,
            (msTimeout < 0) ? portMAX_DELAY : (msTimeout / portTICK_PERIOD_MS));
        return ret & waitBits;
    }
    EventBits_t waitForOneNoReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdFALSE, pdFALSE, msTimeout);
    }
    EventBits_t waitForAllNoReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdTRUE, pdFALSE, msTimeout);
    }
    EventBits_t waitForOneAndReset(EventBits_t waitBits, int msTimeout) {
        return wait(waitBits, pdFALSE, pdTRUE, msTimeout);
    }
};

class RingBuf
{
protected:
    enum: uint8_t { kFlagHasData = 1, kFlagHasEmpty = 2, kFlagWriteOp = 4,
                    kFlagReadOp = 8, kFlagStop = 16 };
    char* mBuf;
    char* mBufEnd;
    char* mWritePtr;
    char* mReadPtr;
    Mutex mMutex;
    EventGroup mEvents;
    int bufSize() const { return mBufEnd - mBuf; }
    int availableForContigRead()
    {
        if (mWritePtr > mReadPtr) { // none wrapped
            return mWritePtr - mReadPtr;
        } else if (mReadPtr > mWritePtr) { // write wrapped, read didn't
            return mBufEnd - mReadPtr;
        } else {
            return 0;
        }
    }
    int availableForContigWrite()
    {
        if (mWritePtr >= mReadPtr) {
            return mBufEnd - mWritePtr;
        } else {
            return mReadPtr - mWritePtr - 1; // artificially leave 1 byte to distinguish between empty and full
        }
    }
    void commitContigRead(int size)
    {
        rbassert(size <= availableForContigRead());
        mReadPtr += size;
        if (mReadPtr == mBufEnd) {
            mReadPtr = mBuf;
        } else {
            assert(mReadPtr < mBufEnd);
        }
        if (mWritePtr == mReadPtr) {
            mEvents.clearBits(kFlagHasData);
        }
        mEvents.setBits(kFlagReadOp);
    }
    void commitContigWrite(int size)
    {
        rbassert(size <= availableForContigWrite());
        mWritePtr += size;
        if (mWritePtr == mBufEnd) {
            mWritePtr = mBuf;
        } else {
            assert(mWritePtr < mBufEnd);
        }
        if (mWritePtr == mReadPtr) {
            mEvents.clearBits(kFlagHasEmpty);
        }
        mEvents.setBits(kFlagWriteOp);
    }
    // -1: stopped, 0: timeout, 1: event occurred
    int8_t waitFor(uint32_t flag, int msTimeout)
    {
        auto bits = mEvents.waitForOneNoReset(flag|kFlagStop, msTimeout);
        if (bits & kFlagStop) {
            return -1;
        } else if (!bits) { //timeout
            return 0;
        }
        assert(bits == flag);
        return 1;
    }
    int8_t waitAndReset(EventBits_t flag, int msTimeout)
    {
        auto bits = mEvents.waitForOneAndReset(flag | kFlagStop, msTimeout);
        if (bits & kFlagStop) {
            return -1;
        } else if (bits == 0) {
            return 0;
        } else {
            rbassert(bits == flag);
            return 1;
        }
    }
    int contigWrite(char* buf, int size)
    {
        int wlen = std::min(size, availableForContigWrite());
        memcpy(mWritePtr, buf, wlen);
        commitContigWrite(wlen);
        return wlen;
    }
public:
    RingBuf(size_t bufSize)
    : mBuf((char*)malloc(bufSize))
    {
        if (!mBuf) {
            ESP_LOGE("RINGBUF", "Out of memory allocation %zu bytes", bufSize);
            return;
        }
        mBufEnd = mBuf + bufSize;
        mWritePtr = mReadPtr = mBuf;
        mEvents.setBits(kFlagHasEmpty);
        assert(mEvents.get() == kFlagHasEmpty);
    }
    int totalDataAvail()
    {
        if (mReadPtr < mWritePtr) {
            return mWritePtr - mReadPtr;
        } else if (mReadPtr > mWritePtr) {
            return (mBufEnd - mReadPtr) + (mWritePtr - mBuf);
        } else { // either empty or full
            return (mEvents.get() & kFlagHasEmpty) ? 0 : (mBufEnd - mBuf);
        }
    }
    int totalEmptySpace()
    {
        return (mBufEnd - mBuf) - totalDataAvail();
    }
    int8_t read(char* buf, int size, int msTimeout)
    {
        for (;;) {
            int64_t tsStart = esp_timer_get_time();
            mMutex.lock();
            if (totalDataAvail() >= size) {
                break;
            }
            mMutex.unlock();
            auto ret = waitAndReset(kFlagWriteOp, msTimeout);
            if (ret <= 0) {
                return ret;
            }
            if (msTimeout > 0) {
                msTimeout -= (esp_timer_get_time() - tsStart) / 1000;
                if (msTimeout < 0) {
                    return 0;
                }
            }
        }
        // mutex is locked here
        auto rlen = std::min(size, availableForContigRead());
        memcpy(buf, mReadPtr, rlen);
        commitContigRead(rlen);
        if (rlen >= size) {
            rbassert(rlen == size);
            mMutex.unlock();
            return 1;
        }
        buf += rlen;
        size -= rlen;
        memcpy(buf, mReadPtr, size);
        commitContigRead(size);
        mMutex.unlock();
        return 1;
    }
    int contigRead(char*& buf, int sizeWanted, int msTimeout)
    {
        auto ret = waitFor(kFlagHasData, msTimeout);
        if (ret <= 0) {
            return ret;
        }
        {
            MutexLocker locker(mMutex);
            auto rlen = std::min(sizeWanted, availableForContigRead());
            buf = mReadPtr;
            commitContigRead(rlen);
            return rlen;
        }
    }
    bool write(char* buf, int size)
    {
        for (;;) {
            mMutex.lock();
            if (totalEmptySpace() >= size) {
                break;
            }
            mMutex.unlock();
            if (waitAndReset(kFlagReadOp, -1) < 0) {
                return false;
            }
        }
        // mutex is locked here
        auto written = contigWrite(buf, size);
        if (written < size) {
            buf += written;
            size -= written;
            written = contigWrite(buf, size);
            rbassert(written == size);
        }
        mMutex.unlock();
        return true;
    }
    int getWriteBuf(char*& buf)
    {
        for (;;) {
            auto ret = waitFor(kFlagHasEmpty, -1);
            if (ret <= 0) {
                return -1;
            }
            {
                MutexLocker locker(mMutex);
                auto contig = availableForContigWrite();
                if (contig < 1) {
                    continue;
                }
                buf = mWritePtr;
                return contig;
            }
        }
    }
    void commitWrite(int size) {
        MutexLocker locker(mMutex);
        commitContigWrite(size);
    }
    void setStopSignal()
    {
        mEvents.setBits(kFlagStop);
    }
    void clearStopSignal()
    {
        mEvents.clearBits(kFlagStop);
    }
};

#endif
