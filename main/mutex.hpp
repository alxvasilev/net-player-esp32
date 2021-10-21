#ifndef MUTEX_HPP_INCLUDED
#define MUTEX_HPP_INCLUDED

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Mutex
{
    SemaphoreHandle_t mMutex;
    StaticSemaphore_t mMutexMem;
public:
    Mutex() {
        mMutex = xSemaphoreCreateRecursiveMutexStatic(&mMutexMem);
    }
    void lock() { xSemaphoreTakeRecursive(mMutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGiveRecursive(mMutex); }
};

class MutexLocker
{
    Mutex& mMutex;
public:
    MutexLocker(Mutex& aMutex): mMutex(aMutex) { mMutex.lock(); }
    ~MutexLocker() { mMutex.unlock(); }
};

class MutexUnlocker
{
    Mutex& mMutex;
public:
    MutexUnlocker(Mutex& aMutex): mMutex(aMutex) { mMutex.unlock(); }
    ~MutexUnlocker() { mMutex.lock(); }
};

#endif
