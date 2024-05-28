#ifndef STREAM_PACKETS_HPP
#define STREAM_PACKETS_HPP

#include "streamDefs.hpp"
#include <memory>
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
        kFlag24BitSamples = 1 << 5,
        kFlagLeftAlignedSamples = 1 << 6,
        kFlagCustomAlloc = 1 << 7  // LSB bits denote the memory block size, in kB
    };
    typedef uint32_t AlignAs;  // 4-byte aligned
    StreamEvent type;
    uint8_t flags;
    void destroy() { // may use custom allocation
        if (flags & kFlagCustomAlloc) {
            //printf("freeing packet type %d of size %d\n", this->type, (this->type == 0) ? *((int16_t*)(((char*)this) + 2)) : -1);
            free(this); //delete[] (AlignAs(*)[])this;
        }
        else {
            delete this;
        }
    }
    template<class T>
    static T* allocWithDataSize(StreamEvent type, size_t dataSize) {
        auto inst = (T*)heap_caps_malloc(sizeof(T) + dataSize, MALLOC_CAP_SPIRAM); //new AlignAs[sizeToArraySize<AlignAs>(sizeof(T) + dataSize)];
        inst->type = type;
        inst->flags = kFlagCustomAlloc;
        return inst;
    }
};
struct TitleChangeEvent: public StreamPacket {
    typedef std::unique_ptr<TitleChangeEvent, Deleter> unique_ptr;
    char title[];
    static TitleChangeEvent* create(const char* aTitle) {
        auto len = strlen(aTitle) + 1;
        auto inst = allocWithDataSize<TitleChangeEvent>(kEvtTitleChanged, len);
        memcpy(inst->title, aTitle, len);
        return inst;
    }
protected:
    TitleChangeEvent() = delete;
};
struct DataPacket: public StreamPacket {
    typedef std::unique_ptr<DataPacket, Deleter> unique_ptr;
    int16_t dataLen;
    alignas(uint32_t) char data[];
    static DataPacket* create(int dataSize) {
        auto inst = allocWithDataSize<DataPacket>(kEvtData, dataSize);
        inst->dataLen = dataSize;
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
            printf("  [%04d]  %s\n", pos, hex.get());
        }
    }
protected:
    DataPacket() = delete;
};
struct GenericEvent: public StreamPacket {
    typedef std::unique_ptr<StreamPacket, Deleter> unique_ptr;
    uint8_t streamId;
    uint8_t sourceBps;
    StreamFormat fmt;
    GenericEvent(StreamEvent aType, uint8_t aStreamId, StreamFormat aFmt, uint8_t aSourceBps=0)
        :StreamPacket(aType, 0), streamId(aStreamId), sourceBps(aSourceBps), fmt(aFmt)  {}
};
#endif
