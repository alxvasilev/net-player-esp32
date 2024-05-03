#include <ringbuf.hpp>
#include <staticQueue.hpp>
#include "audioNode.hpp"
#include "streamEvents.hpp"

enum { kMaxEventCount = 6 };
class RingBufWithEvents: public RingBuf {
protected:
    using RingBuf::peek;
    using RingBuf::commitWrite;
    int64_t mWritePos = 0;
    StaticQueue<StreamEvent*, kMaxEventCount> mEventQueue;
    int64_t readPos() { return mWritePos - dataSize(); }
public:
    int64_t rxByteCtr() { return mWritePos; }
    typedef bool(*EventCb)(StreamEvent&, void* userp);
    RingBufWithEvents(size_t size, bool useSpiRam): RingBuf(size, useSpiRam){}
    void clear();
    AudioNode::StreamError pullData(AudioNode::DataPullReq& dpr, EventCb onEvent=nullptr, void* userp=nullptr);
    StreamEvent* dequeuePendingEvent();
    const char* peek(int len, char* buf, uint8_t ignoredEvents);
    void commitWrite(int size) {
        MutexLocker locker(mMutex);
        mWritePos += size;
        RingBuf::commitWrite(size); // locks recursively mMutex
    }
    bool postEvent_Lock(StreamEvent* event);
    bool postEvent_NoLock(StreamEvent* event);
};
