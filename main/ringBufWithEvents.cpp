#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <strings.h>
#include "ringBufWithEvents.hpp"
#include "utils.hpp"

#define LOCK() MutexLocker locker(mMutex)

void RingBufWithEvents::clear()
{
    LOCK();
    RingBuf::clear();
    mEventQueue.iterate([](StreamEvent* event) {
        delete event;
        return true;
    });
    mEventQueue.clear();
}

const char* RingBufWithEvents::peek(int len, char* buf, uint8_t ignoredEvents)
{
    LOCK();
    bool ok = true;
    auto till = readPos() + len;
    mEventQueue.iterate([&ok, till, ignoredEvents, this](StreamEvent* event) {
        if (event->streamPos >= till) {
            return false;
        }
        else {
            if (event->type & ignoredEvents) {
                return true;
            }
            ok = false;
            return false;
        }
    });
    if (!ok) {
        return nullptr;
    }
    while (dataSize() < len) {
        MutexUnlocker unlocker(mMutex);
        if (waitForData(-1) <= 0) {
            return nullptr;
        }
    }
    return RingBuf::peek(len, buf, 0);
}

AudioNode::StreamError RingBufWithEvents::pullData(AudioNode::DataPullReq& dpr, EventCb onEvent, void* userp)
{
    /* Dequeue immediate pending events, and return data only up to a further pending event.
     * If the ringbuf is empty, wait for data, and re-check for newly queued events before proceeding
     * with returning the data. This ensures that we can't have events missed while waiting for data,
     * which would othwrwise be returned _after_ the data
     */
    LOCK();
    auto startPos = readPos();
    for (;;) {
        for (;;) {
            StreamEvent** first = mEventQueue.front();
            if (!first) { // no events - return data
                break;
            }
            StreamEvent& event = **first;
            auto eventPos = event.streamPos;
            if (eventPos > startPos + dpr.size) { // event is not within the read window
                break;
            }
            if (eventPos <= startPos) { // event is right at out read pos, return it
                mEventQueue.popFront();
                bool cont = onEvent ? onEvent(event, userp) : false;
                if (cont) { // event dispatched, dequeue it and continue with next event, if any
                    continue;
                }
                dpr.setEvent(event);
                return event.type;
            }
            // there is an event within the read window
            bool cont = onEvent ? onEvent(event, userp) : false;
            if (cont) { // event dispatched, dequeue it and continue with next event, if any
                mEventQueue.popFront();
                continue;
            }
            // return buffer up till this event, and don't dequeue the event
            dpr.size = eventPos - startPos;
            break;
        }
        if (!dataSize()) {
            MutexUnlocker unlocker(mMutex);
            auto ret = waitForData(-1);
            if (ret < 0) {
                dpr.size = 0;
                return AudioNode::kErrStreamStopped;
            }
            continue;
        }
        break;
    }
    // return requested amount of data
    myassert(dataSize() > 0);
    auto ret = contigRead(dpr.buf, dpr.size, -1);
    if (ret < 0) { // just in case
        myassert(!dpr.buf);
        dpr.size = 0;
        return AudioNode::kErrStreamStopped;
    }
    printf("ret = %d\n", ret);
    myassert(ret > 0); // no timeout
    dpr.size = ret;
    return AudioNode::kNoError;
}
StreamEvent* RingBufWithEvents::dequeuePendingEvent() {
    LOCK();
    if (mEventQueue.empty()) {
        return nullptr;
    }
    auto evt = *mEventQueue.front();
    if (evt->streamPos <= readPos()) {
        mEventQueue.popFront();
        return evt;
    }
    return nullptr;
}
bool RingBufWithEvents::postEvent_Lock(StreamEvent* event) {
    if (event->streamPos < 0) {
        event->streamPos = mWritePos;
    }
    for(;;) {
        bool ok;
        {
            MutexLocker locker(mMutex);
            ok = mEventQueue.emplaceBack(event);
        }
        if (ok) {
            return true;
        }
        if (waitForReadOp(-1) < 0) {
            return false;
        }
    }
}
bool RingBufWithEvents::postEvent_NoLock(StreamEvent* event) {
    if (event->streamPos < 0) {
        event->streamPos = mWritePos;
    }
    while (!mEventQueue.emplaceBack(event)) {
        MutexUnlocker unlock(mMutex);
        if (waitForReadOp(-1) < 0) {
            return false;
        }
    }
    return true;
}
