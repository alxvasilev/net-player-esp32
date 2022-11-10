#ifndef STATICQUEUE_HPP
#define STATICQUEUE_HPP
#include <assert.h>
#include <type_traits>
#include <new>

template <class T, int N>
class StaticQueue {
protected:
    alignas(T[]) mutable char mData[sizeof(T[N])];
    T* items() const { return reinterpret_cast<T*>(mData); }
    /* mStart == mEnd -> wrapped, list is full
     * mStart < mEnd -> not wrapped, may be empty if mStart < 0
     * mStart > mEnd -> wrapped, not full
     */
    int mStart = -1;
    int mEnd = 0;
public:
    class Popper {
        StaticQueue& mQueue;
    public:
        Popper(StaticQueue& q): mQueue(q) {}
        ~Popper() { mQueue.popFront(); }
    };
    bool empty() const { return mStart < 0; }
    bool full() const { return mEnd == mStart; }
    T* front() const { return empty() ? nullptr : items() + mStart; }
    T* back() const {
        if (empty()) {
            return nullptr;
        }
        return (mEnd == 0) ? items() + (N-1) : items() + (mEnd-1);
    }
    template <typename... TArgs>
    bool emplaceBack(TArgs... args) {
        if (full()) {
            return false;
        }
        if (empty()) {
            mStart = 0;
            assert(mEnd == 0);
        }
        new (items()+(mEnd++)) T(args...);
        if (mEnd >= N) {
            mEnd = 0;
        }
        return true;
    }
    bool popFront() {
        if (empty()) {
            return false;
        }
        items()[mStart].~T();
        if (++mStart >= N) {
            mStart = 0;
        }
        if (mStart == mEnd) {
            mStart = -1;
            mEnd = 0;
        }
        return true;
    }
    void clear() {
        iterate([](T& item) { item.~T(); });
        mStart = -1;
        mEnd = 0;
    }
    template <class CB>
    void iterate(CB&& cb) {
        if (empty()) {
            return;
        }
        if (mStart < mEnd) {
            for (int idx = mStart; idx < mEnd; idx++) {
                cb(items()[idx]);
            }
        } else {
            for (int idx = mStart; idx < N; idx++) {
                cb(items()[idx]);
            }
            for (int idx = 0; idx < mEnd; idx++) {
                cb(items()[idx]);
            }
       }
    }
    ~StaticQueue() {
        clear();
    }
};

#endif // STATICQUEUE_HPP
