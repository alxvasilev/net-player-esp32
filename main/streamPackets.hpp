#ifndef STREAM_PACKETS_HPP
#define STREAM_PACKETS_HPP

#include "streamDefs.hpp"
#include <memory>
#include <atomic>
#include "utils.hpp"
#include <utils-parse.hpp>

constexpr uint32_t myLog2(uint32_t n) noexcept
{
    return (n > 1) ? 1 + myLog2(n >> 1) : 0;
}
template<typename T>
int sizeToArraySize(int memSize) {
    constexpr int shift = myLog2(sizeof(T));
    return (memSize + sizeof(T) - 1) >> shift;
}

struct StreamPacket {
protected:
    StreamPacket(StreamEvent aType, uint8_t aFlags): type(aType), flags(aFlags) {}
    struct Deleter { void operator()(StreamPacket* pkt) { pkt->destroy(); } };
public:
    struct unique_ptr: public std::unique_ptr<StreamPacket, Deleter> {
        using std::unique_ptr<StreamPacket, Deleter>::unique_ptr;
        template <class T>
        T& as() const { return *(T*)get(); }
    };
    enum: uint8_t {
        kFlagHasSpaceFor32Bit = 1 << 0,
        kFlagLeftAlignedSamples = 1 << 1,
        kFlagCustomAlloc = 1 << 2
    };
    StreamEvent type;
    uint8_t flags;
    void destroy() { // may use custom allocation
        if (flags & kFlagCustomAlloc) {
            free(this);
        }
        else {
            delete this;
        }
    }
    template<class T>
    static T* allocWithDataSize(StreamEvent type, size_t dataSize) {
        auto inst = (T*)heap_caps_malloc(sizeof(T) + dataSize, MALLOC_CAP_SPIRAM);
        inst->type = type;
        inst->flags = kFlagCustomAlloc;
        return inst;
    }
};
struct TitleChangeEvent: public StreamPacket {
    typedef std::unique_ptr<TitleChangeEvent, Deleter> unique_ptr;
    unique_ptr_mfree<const char> title;
    unique_ptr_mfree<const char> artist;
    TitleChangeEvent(const char* aTitle, const char* aArtist, uint8_t flags=0)
    : StreamPacket(kEvtTitleChanged, flags), title(aTitle), artist(aArtist)
    {}
};
struct DataPacket: public StreamPacket {
    typedef std::unique_ptr<DataPacket, Deleter> unique_ptr;
    int16_t dataLen;
    alignas(uint32_t) char data[];
    template <bool Empty=false>
    static DataPacket* create(int bufSize) {
        auto inst = allocWithDataSize<DataPacket>(kEvtData, bufSize);
        inst->dataLen = Empty ? 0 : bufSize;
        return inst;
    }
    void logData(int16_t maxLen, const char* msg, int lineLen=20)
    {
        int len = std::min(maxLen, dataLen);
        std::unique_ptr<char[]> hex(new char[len * 3 + 10]);
        printf("%s(%d of %d):\n", msg, len, dataLen);
        for (int pos = 0; pos < len; pos += lineLen) {
            auto thisLineLen = std::min(lineLen, len - pos);
            binToHex((uint8_t*)data + pos, thisLineLen, hex.get());
            printf("  [%04d]  %s ", pos, hex.get());
            auto delta = lineLen - thisLineLen;
            if (delta) {
                delta *= 3;
                for (int i = 0; i < delta; i++) {
                    putchar(' ');
                }
            }
            for (int i = pos, end = pos + thisLineLen; i < end; i++) {
                char ch = data[i];
                putchar((ch < 32 || ch > 126) ? '.' : ch);
            }
            putchar('\n');
        }
    }
protected:
    DataPacket() = delete;
};
struct GenericEvent: public StreamPacket {
    typedef std::unique_ptr<GenericEvent, Deleter> unique_ptr;
    StreamId streamId;
    GenericEvent(StreamEvent aType, StreamId aStreamId, uint8_t aFlags)
    : StreamPacket(aType, aFlags), streamId(aStreamId) {}
};

struct NewStreamEvent: public GenericEvent {
    typedef std::unique_ptr<NewStreamEvent, Deleter> unique_ptr;
    StreamFormat fmt;
    uint32_t seekTime;
    uint8_t sourceBps;
    NewStreamEvent(StreamId aStreamId, StreamFormat aFmt, uint8_t aSourceBps=0, uint32_t aSeekTime=0)
    : GenericEvent(kEvtStreamChanged, aStreamId, 0), fmt(aFmt), seekTime(aSeekTime), sourceBps(aSourceBps) {}
};
struct StreamEndEvent: public GenericEvent {
    StreamEndEvent(StreamId streamId): GenericEvent(kEvtStreamEnd, streamId, 0) {}
};
struct PrefillEvent: public StreamPacket {
public:
    typedef int16_t IdType;
    static IdType lastPrefillId() { return sLastPrefillId; }
    IdType prefillId;
    PrefillEvent(uint8_t flags=0): StreamPacket(kEvtPrefill, flags), prefillId(++sLastPrefillId) {}
protected:
    inline static std::atomic<IdType> sLastPrefillId {0};
};
#endif
