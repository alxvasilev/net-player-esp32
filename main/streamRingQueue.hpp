#ifndef STREAMRINGQUEUE_HPP
#define STREAMRINGQUEUE_HPP

#include <ringQueue.hpp>
#include <mutex.hpp>
#include <waitable.hpp>
#include "streamPackets.hpp"

template<int N>
class StreamRingQueue: protected RingQueue<StreamPacket*, N>, public Waitable {
protected:
    typedef RingQueue<StreamPacket*, N> Base;
    enum { kFlagHasData = kFlagLast << 1 };
    int mDataSize = 0;
public:
    mutable Mutex mMutex;
    using Base::capacity;
    using Base::empty; // doesn't need locking - is a single primtive member read
    int size() const {
        MutexLocker locker(mMutex);
        return Base::size();
    }
    bool full() const {
        MutexLocker locker(mMutex);
        return Base::full();
    }
    template <class CB>
    void iterate(CB&& cb) {
        MutexLocker locker(mMutex);
        Base::iterate(std::forward<CB>(cb));
    }
    /* The packets returned by the peekXXX methods cannot be destroyed even if the queue is not locked,
     * unless someone else reads the queue meanwhile.
     */
    StreamPacket* peekFirstOfType(StreamEvent type, StreamEvent cantBePrecededBy=kInvalidStreamEvent) {
        StreamPacket* result = nullptr;
        if (cantBePrecededBy != kInvalidStreamEvent) {
            iterate([type, cantBePrecededBy, &result](StreamPacket*& pkt) {
                auto pktType = pkt->type;
                if (pktType == type) {
                    result = pkt;
                    return false;
                }
                else if (pktType & cantBePrecededBy) {
                    return false;
                }
                else {
                    return true;
                }
            });
        }
        else {
            iterate([type, &result](StreamPacket*& pkt) {
                if (pkt->type == type) {
                    result = pkt;
                    return false;
                }
                return true;
            });
        }
        return result;
    }
    int8_t waitForData(int timeout) {
        return waitFor(kFlagHasData, timeout);
    }
    /** Returns the first data packet found in the queue. If canBePrecededBy is specified, and any other
     *  than that type of packet precedes the data packet, null is returned. This is useful to peek the
     *  next data packet, but only if it doesn't belong to a new stream.
     *  If canBePrecededBy is kInvalidStreamEvent, no check is done what precedes the data packet.
     *  NOTE: Queue must not be locked before calling this method
     */
    DataPacket* peekFirstDataWait(StreamEvent cantBePrecededBy=kInvalidStreamEvent, bool* precByOther = nullptr) {
        MutexLocker locker(mMutex);
        while(mDataSize <= 0) {
            MutexUnlocker unlocker(mMutex);
            int ret = waitForData(-1);
            if (ret < 0) {
                if (precByOther) {
                    *precByOther = false;
                }
                return nullptr;
            }
        }
        auto pkt = (DataPacket*)peekFirstOfType(kEvtData, cantBePrecededBy);
        if (precByOther) {
            *precByOther = !pkt;
        }
        return pkt;
    }
    StreamPacket* peekFirstWait() {
        MutexLocker locker(mMutex);
        while (Base::empty()) {
            MutexUnlocker unlocker(mMutex);
            int ret = waitForItems(-1);
            if (ret < 0) {
                return nullptr;
            }
        }
        return front();
    }
    int dataSize() const { return mDataSize; }
    /** Must not be locked */
    bool pushBack(StreamPacket* item) {
        MutexLocker locker(mMutex);
        while (Base::full()) {
            MutexUnlocker unlocker(mMutex);
            auto ret = waitForReadOp(-1);
            if (ret <= 0) {
                delete item;
                return false;
            }
        }
        bool ok = Base::emplaceBack(item);
        assert(ok);

        EventBits_t bitsToSet = kFlagWriteOp | kFlagHasItems;
        if (item->type == kEvtData) {
            auto prevData = mDataSize;
            mDataSize += static_cast<DataPacket*>(item)->dataLen;
            if ((prevData == 0) && (mDataSize > 0)) {
                bitsToSet |= kFlagHasData;
            }
        }
        EventBits_t bitsToClear = kFlagIsEmpty;
        if (Base::full()) {
            bitsToClear |= kFlagHasSpace;
        }
        mEvents.clearBits(bitsToClear);
        mEvents.setBits(bitsToSet);
        return true;
    }
    StreamPacket* front() {
        MutexLocker locker(mMutex);
        auto frontPkt = Base::front();
        return frontPkt ? *frontPkt : nullptr;
    }
    /** Must not be locked */
    StreamPacket* popFront() {
        MutexLocker locker(mMutex);
        while (Base::empty()) {
            MutexUnlocker unlocker(mMutex);
            if (waitForWriteOp(-1) <= 0) {
                return nullptr;
            }
        }
        auto item = Base::front();
        assert(item);
        Base::popFront();
        EventBits_t bitsToClear = 0;
        EventBits_t bitsToSet = kFlagReadOp | kFlagHasSpace;
        if ((*item)->type == kEvtData) {
            auto prevData = mDataSize;
            mDataSize -= static_cast<DataPacket*>(*item)->dataLen;
            if ((mDataSize == 0) && (prevData > 0)) {
                bitsToClear |= kFlagHasData;
            }
        }
        if (Base::empty()) {
            assert(mDataSize == 0);
            bitsToClear |= kFlagHasItems;
            bitsToSet |= kFlagIsEmpty;
        }
        mEvents.setBits(bitsToSet);
        if (bitsToClear) {
            mEvents.clearBits(bitsToClear);
        }
        return *item;
    }
    void clear() {
        MutexLocker locker(mMutex);
        Base::iterate([](StreamPacket*& pkt) {
            pkt->destroy();
            return true;
        });
        Base::clear();
        mDataSize = 0;
        mEvents.clearBits(0xff);
        mEvents.setBits(kFlagHasSpace|kFlagIsEmpty|kFlagReadOp);
        assert(mEvents.get() == (kFlagHasSpace|kFlagIsEmpty|kFlagReadOp));
    }
    EventBits_t events() const {
        return mEvents.get();
    }
};
#endif // STREAMRINGQUEUE_HPP
