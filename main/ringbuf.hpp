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
    enum: uint8_t { kFlagHasData = 1, kFlagIsEmpty = 2, kFlagHasEmpty = 4,
                    kFlagWriteOp = 8, kFlagReadOp = 16, kFlagStop = 32 };
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
        } else if (mEvents.get() & kFlagHasEmpty) { // empty
            return 0;
        } else { // full
            myassert(mEvents.get() & kFlagHasData);
            return mBufEnd - mReadPtr;
        }
    }
    int maxPossibleContigReadSize() { return mBufEnd - mReadPtr; }
    int availableForContigWrite()
    {
        if (mWritePtr >= mReadPtr) {
            return mBufEnd - mWritePtr;
        } else {
            return mReadPtr - mWritePtr - 1; // artificially leave 1 byte to distinguish between empty and full
        }
    }
    void doCommitContigRead(int size)
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
            mEvents.setBits(kFlagIsEmpty);
        }
        mEvents.setBits(kFlagReadOp|kFlagHasEmpty);
    }
    void commitContigWrite(int size)
    {
        rbassert(size <= availableForContigWrite());
        mWritePtr += size;
        if (mWritePtr == mBufEnd) {
            mWritePtr = mBuf;
        } else {
            rbassert(mWritePtr < mBufEnd);
        }
        EventBits_t bitsToClear = kFlagIsEmpty;
        if (mWritePtr == mReadPtr) {
            bitsToClear |= kFlagHasEmpty;
        }
        mEvents.clearBits(bitsToClear);
        mEvents.setBits(kFlagWriteOp | kFlagHasData);
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
    int totalDataAvail_nolock()
    {
        if (mReadPtr < mWritePtr) {
            return mWritePtr - mReadPtr;
        } else if (mReadPtr > mWritePtr) {
            return (mBufEnd - mReadPtr) + (mWritePtr - mBuf);
        } else { // either empty or full
            return (mEvents.get() & kFlagHasEmpty) ? 0 : (mBufEnd - mBuf);
        }
    }
    int totalEmptySpace_nolock()
    {
        return (mBufEnd - mBuf) - totalDataAvail();
    }
public:
    // If user wants to keep some external state in sync with the ringbuffer,
    // they can use the ringbuf's mutex to protect that state
    Mutex& mutex() { return mMutex; }
    RingBuf(size_t bufSize)
    : mBuf((char*)malloc(bufSize))
    {
        if (!mBuf) {
            ESP_LOGE("RINGBUF", "Out of memory allocation %zu bytes", bufSize);
            return;
        }
        mBufEnd = mBuf + bufSize;
        mWritePtr = mReadPtr = mBuf;
        mEvents.setBits(kFlagHasEmpty|kFlagIsEmpty);
        assert(mEvents.get() == (kFlagHasEmpty|kFlagIsEmpty));
    }
    int totalDataAvail()
    {
        MutexLocker locker(mMutex);
        return totalDataAvail_nolock();
    }
    int totalEmptySpace()
    {
        MutexLocker locker(mMutex);
        return totalEmptySpace_nolock();
    }
    /* Read requested amount and block if needed.
     * @returns 1 upon success, 0 upon timeout, -1 if stop was signalled
     */
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
        doCommitContigRead(rlen);
        if (rlen >= size) {
            rbassert(rlen == size);
            mMutex.unlock();
            return 1;
        }
        buf += rlen;
        size -= rlen;
        memcpy(buf, mReadPtr, size);
        doCommitContigRead(size);
        mMutex.unlock();
        return 1;
    }
    /* Returns a contiguous buffer with data for reading, which may be shorter
     * than sizeWanted. If no data is available for reading, blocks until data becomes
     * available or timeout elapses
     * @returns the amount of data in the returned buffer, 0 for timeout or -1 if stop
     * was signalled
     */
    int contigRead(char*& buf, int sizeWanted, int msTimeout)
    {
        MutexLocker locker(mMutex);
        auto maxPossible = maxPossibleContigReadSize();
        if (sizeWanted > maxPossible) {
            sizeWanted = maxPossible;
        }
        if (msTimeout < 0) {
            while (availableForContigRead() < sizeWanted) {
                MutexUnlocker unlocker(mMutex);
                if (waitFor(kFlagWriteOp, -1) <= 0) {
                    return -1;
                }
            }
        } else {
            while (availableForContigRead() < sizeWanted) {
                int64_t tsStart = esp_timer_get_time();
                MutexUnlocker unlocker(mMutex);
                int ret = waitFor(kFlagWriteOp, msTimeout);
                msTimeout -= (esp_timer_get_time() - tsStart) / 1000;
                if (ret < 0) {
                    return ret;
                }
                if (msTimeout < 0) {
                    return 0;
                }
            }
        }
        buf = mReadPtr;
        return sizeWanted;
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
    int getWriteBuf(char*& buf, int reqSize)
    {
        MutexLocker locker(mMutex);
        int maxPossible = mBufEnd - mWritePtr;
        if (reqSize > maxPossible) {
            reqSize = maxPossible;
        }
        for (;;) {
            {
                MutexUnlocker unlocker(mMutex);
                auto ret = waitFor(kFlagHasEmpty, -1);
                if (ret <= 0) {
                    return -1;
                }
            }
            auto contig = availableForContigWrite();
            if (contig < reqSize) {
                continue;
            }
            buf = mWritePtr;
            return contig;
        }
    }
    void commitWrite(int size) {
        MutexLocker locker(mMutex);
        commitContigWrite(size);
    }
    void commitContigRead(int size)
    {
        MutexLocker locker(mMutex);
        doCommitContigRead(size);

    }
    void setStopSignal()
    {
        mEvents.setBits(kFlagStop);
    }
    void clearStopSignal()
    {
        mEvents.clearBits(kFlagStop);
    }
    bool hasData() const
    {
        return ((mEvents.get() & kFlagHasData) != 0);
    }
    bool waitForData()
    {
        return waitFor(kFlagHasData, -1) >= 0;
    }
    bool waitForEmpty()
    {
        return waitFor(kFlagIsEmpty, -1) >= 0;
    }

};

#endif
