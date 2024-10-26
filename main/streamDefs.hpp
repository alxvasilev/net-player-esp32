#ifndef STREAM_DEFS_HPP
#define STREAM_DEFS_HPP
#include <stdint.h>

struct Codec {
    enum Type: uint8_t {
        kCodecUnknown = 0,
        kCodecMp3,
        kCodecAac,
        kCodecVorbis,
        kCodecFlac,
        kCodecOpus,
        kCodecWav,
        kCodecPcm,
        // ====
        kPlaylistM3u8,
        kPlaylistPls
    };
    enum Transport: uint8_t {
        kTransportDefault = 0,
        kTransportOgg  = 1,
        kTransportMpeg = 2
    };
    enum Mode: uint8_t {
        kAacModeSbr    = 1
    };
    union {
        struct {
            Type type: 4;
            Mode mode: 2;
            Transport transport: 2;
        };
        uint8_t numCode;
    };
    Codec(uint8_t val = 0): numCode(val) {
        static_assert(sizeof(Codec) == 1, "sizeof(Codec) must be 1 byte");
    }
    Codec(Type aType, Transport aTrans): numCode(0) {
        type = aType;
        transport = aTrans;
    }
    const char* toString() const;
    const char* fileExt() const;
    operator uint8_t() const { return this->type; }
    operator bool() const { return this->type != 0; }
    Codec& operator=(Type aType) { type = aType; return *this; }
    uint8_t asNumCode() const { return numCode; }
    void fromNumCode(uint8_t val) { numCode = val; }
    static const char* numCodeToStr(uint8_t aNumCode) {
        Codec inst(aNumCode); return inst.toString();
    }
    void clear() { numCode = 0; }
};

typedef uint8_t StreamId;
struct StreamFormat
{
protected:
    struct Members {
        uint32_t sampleRate: 19;
        uint8_t numChannels: 1;
        uint8_t bitsPerSample: 2;
        bool isLeftAligned: 1;
        bool isBigEndian: 1;
        Codec codec;
    };
    union {
        Members members;
        uint32_t mNumCode;
    };
    void initSampleFormat(uint32_t sr, uint8_t bps, uint8_t channels)
    {
        setSampleRate(sr);
        setBitsPerSample(bps);
        setNumChannels(channels);
    }
public:
    operator bool() const { return mNumCode != 0; }
    static uint8_t encodeBps(uint8_t bits) { return (bits >> 3) - 1; }
    static uint8_t decodeBps(uint8_t bits) { return (bits + 1) << 3; }
    void clear() { mNumCode = 0; }
    StreamFormat(uint32_t code): mNumCode(code) {}
    StreamFormat(uint32_t sr, uint8_t bps, uint8_t channels): mNumCode(0)
    {
        initSampleFormat(sr, bps, channels);
    }
    StreamFormat(Codec codec): mNumCode(0) { members.codec = codec; }
    StreamFormat(Codec::Type codec): mNumCode(0) { members.codec = codec; }
    StreamFormat(Codec::Type codec, uint32_t sr, uint8_t bps, uint8_t channels): mNumCode(0)
    {
        initSampleFormat(sr, bps, channels);
        members.codec = codec;
    }
    StreamFormat(): mNumCode(0)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "Size of StreamFormat must be 32bit");
    }
    bool operator==(StreamFormat other) const { return mNumCode == other.mNumCode; }
    bool operator!=(StreamFormat other) const { return mNumCode != other.mNumCode; }
    uint32_t asNumCode() const { return mNumCode; }
    static StreamFormat fromNumCode(uint32_t code) { return StreamFormat(code); }//{.mNumCode = code}; }
    static StreamFormat fromMimeType(const char* mime);
    static StreamFormat parseLpcmContentType(const char* ctype, int bps);
    void set(uint32_t sr, uint8_t bits, uint8_t channels) {
        setNumChannels(channels);
        setSampleRate(sr);
        setBitsPerSample(bits);
    }
    const Codec& codec() const { return members.codec; }
    Codec& codec() { return members.codec; }
    void setCodec(Codec codec) { members.codec = codec; }
    uint32_t sampleRate() const { return members.sampleRate; }
    void setSampleRate(uint32_t sr) { members.sampleRate = sr; }
    uint8_t bitsPerSample() const { return decodeBps(members.bitsPerSample); }
    void setBitsPerSample(uint8_t bps) { members.bitsPerSample = encodeBps(bps); }
    uint8_t numChannels() const { return members.numChannels + 1; }
    bool isStereo() const { return members.numChannels != 0; }
    bool isLeftAligned() const { return members.isLeftAligned; }
    bool isBigEndian() const { return members.isBigEndian; }
    void setBigEndian(bool isBe) { members.isBigEndian = isBe; }
    void setIsLeftAligned(bool val) { members.isLeftAligned = val; }
    void setNumChannels(uint8_t ch) { members.numChannels = ch - 1; }
    int prefillAmount() const;
    int16_t netRecvSize() const;
};

enum StreamEvent: int8_t {
    kNoError = 0, // used only when StreamEvent is used for custom signalling
    kEvtData = 0,
    kEvtStreamEnd = 1,
    kEvtStreamChanged = 2,
    kEvtSeek = 3,
    kEvtTitleChanged = 4,
    kErrStreamStopped = -1,
    kErrTimeout = -2,
    kErrNoCodec = -3,
    kErrDecode = -4,
    kErrStreamFmt = -5,
    kErrNotFound = -6,
    kInvalidStreamEvent = -128 // used for optional params
};
const char* streamEventToStr(StreamEvent evt);

#endif
