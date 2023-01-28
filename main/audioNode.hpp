#ifndef AUDIO_NODE_HPP
#define AUDIO_NODE_HPP

#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "playlist.hpp"
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
    static const char* numCodeToStr(uint8_t aNumCode) {
        Codec inst = { .numCode = aNumCode }; return inst.toString();
    }
    void clear() { numCode = 0; }
};

struct StreamFormat
{
protected:
    struct Members {
        uint32_t sampleRate: 19;
        uint8_t numChannels: 1;
        uint8_t bitsPerSample: 2;
        bool isLeftAligned: 1;
        uint8_t reserved: 1;
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
    StreamFormat(uint32_t sr, uint8_t bps, uint8_t channels): mNumCode(0)
    {
        initSampleFormat(sr, bps, channels);
    }
    StreamFormat(Codec codec): mNumCode(0) { members.codec = codec; }
    StreamFormat(Codec::Type type): mNumCode(0) { members.codec.type = type; }
    StreamFormat(Codec codec, uint32_t sr, uint8_t bps, uint8_t channels): mNumCode(0)
    {
        initSampleFormat(sr, bps, channels);
        members.codec = codec;
    }
    StreamFormat(): mNumCode(0)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "Size of StreamFormat must be 32bit");
    }
    StreamFormat(uint32_t code): mNumCode(code) {}
    bool operator==(StreamFormat other) const { return mNumCode == other.mNumCode; }
    bool operator!=(StreamFormat other) const { return mNumCode != other.mNumCode; }
    uint32_t asNumCode() const { return mNumCode; }
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
    void setIsLeftAligned(bool val) { members.isLeftAligned = val; }
    void setNumChannels(uint8_t ch) { members.numChannels = ch - 1; }
};

class AudioNode;

class IAudioPipeline {
public:
    virtual bool onNodeEvent(AudioNode& node, uint32_t type, size_t numArg, uintptr_t arg) = 0;
    virtual void onNodeError(AudioNode& node, int error) = 0;
};

class IAudioVolume;

class AudioNode
{
public:
    enum StreamError: int8_t {
        kNoError = 0,
        kTimeout,
        kStreamStopped,
        kStreamEnd,
        kStreamChanged,
        kCodecChanged,
        kTitleChanged,
        kErrNoCodec,
        kErrDecode,
        kErrStreamFmt
    };
    enum { kEventStreamError = 1, kEventAudioFormatChange, kEventNewStream, kEventLast = kEventNewStream };
    // we put here the state definitions only because the class name is shorter than AudioNodeWithTask
    enum State: uint8_t {
        kStateTerminated = 1, kStateStopped = 2,
        kStateRunning = 4, kStateLast = kStateRunning
    };
    // Static registry of all possible node types.
    // Ideally each node class should have a static singleton that should be
    // initialized at runtime with an autoincremented global that is a static member
    // of this class
    enum Type: uint8_t {
        kTypeUnknown = 0,
        kTypeHttpIn = 0x80, // convenient for subcategories
        kTypeA2dpIn = 1,
        kTypeI2sIn = 2,
        kTypeDecoder = 3,
        kTypeEncoder = 4,
        kTypeEqualizer = 5,
        kTypeI2sOut = 6,
        kTypeHttpOut = 7,
        kTypeA2dpOut = 8
    };
    const char* tag() { return mTag; }
protected:
    IAudioPipeline& mPipeline;
    const char* mTag;
    Mutex mMutex;
    AudioNode* mPrev = nullptr;
    inline bool plSendEvent(uint32_t type, size_t numArg = 0, uintptr_t arg=0);
    inline void plNotifyError(int error);
    AudioNode(IAudioPipeline& parent, const char* tag): mPipeline(parent), mTag(tag) {}
public:
    virtual Type type() const = 0;
    virtual IAudioVolume* volumeInterface() { return nullptr; }
    virtual ~AudioNode() {}
    virtual void reset() {}
    virtual bool waitForPrefill() { return true; }
    virtual const char* peek(int size, char* buf) { return nullptr; }
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    AudioNode* prev() const { return mPrev; }
    typedef uint8_t StreamId;
    struct DataPullReq
    {
        char* buf = nullptr;
        int size;
        StreamFormat fmt;
        StreamId streamId;
        DataPullReq(size_t aSize): size(aSize) {}
        void reset(size_t aSize)
        {
            size = aSize;
            buf = nullptr;
            streamId = 0;
            fmt.clear();
            myassert(!fmt.codec());
        }
        void clear() { reset(0); }
        void clearExceptStreamId()
        {
            size = 0;
            buf = nullptr;
            fmt.clear();
        }
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamError pullData(DataPullReq& dpr) = 0;
    virtual void confirmRead(int amount) = 0;
    /** Reads exactly the specified amount of bytes to buf, or discards them
     *  if \c buf is null */
    StreamError readExact(DataPullReq& dpr, int size, char* buf);
    static const char* streamEventToStr(StreamError evt);
    static StreamError threeStateStreamError(int ret) {
        if (ret > 0) {
            return kNoError;
        }
        else if (ret == 0) {
            return kTimeout;
        }
        else {
            return kStreamStopped;
        }
    }
};

class AudioNodeWithState: public AudioNode
{
protected:
    EventGroup mEvents;
    volatile State mState; // state is a backing store for the state in mEvents
    enum { kStateMask = (kStateLast << 1) - 1, kEvtStopRequest = kStateLast << 1, kEvtLast = kEvtStopRequest };
    /** Called internally when the state has been reached */
    void setState(State newState);
    virtual void onStopped() {}
public:
    AudioNodeWithState(IAudioPipeline& parent, const char* tag)
    : AudioNode(parent, tag), mEvents(kEvtStopRequest), mState(kStateTerminated) {
        mEvents.setBits(kStateTerminated);
    }
    State state() const { return mState; }
    State waitForState(unsigned state, int timeout=-1) { return (State)mEvents.waitForOneNoReset(state, timeout); }
    void waitForStop() { waitForState(kStateStopped|kStateTerminated); }
    void waitForTerminate() { waitForState(kStateTerminated); }
    virtual bool run();
    virtual void stop(bool wait=true);
    virtual void terminate(bool wait=true);
    static const char* stateToStr(State state);
};

class AudioNodeWithTask: public AudioNodeWithState
{
public:
protected:
    struct Command
    {
        uint16_t opcode;
        void* arg;
        Command(uint8_t aCmd, void* aArg=nullptr): opcode(aCmd), arg(aArg){}
        Command(){} //no init, used for retrieving commands
    };
    enum: uint8_t { kCommandStop = 1, kCommandRun, kCommandLast = kCommandRun };
    enum { kDefaultPrio = 4 };
    TaskHandle_t mTaskId = NULL;
    uint32_t mStackSize;
    Queue<Command, 4> mCmdQueue;
    bool mTerminate = false;
    uint8_t mTaskPrio;
    int8_t mCpuCore;
    static void sTaskFunc(void* ctx);
    bool createAndStartTask();
    void processMessages();
    // This is the node's task function.
    // It is executed within a MutexLocker scope, and the node's state is set to
    // kStatePaused before it is called. After it returns, the node's state
    // is set to kStateStopped
    virtual void nodeThreadFunc() = 0;
    virtual bool dispatchCommand(Command& cmd);
    virtual void onStopRequest() {}
    virtual void onStopped() {}
public:
    AudioNodeWithTask(IAudioPipeline& parent, const char* tag, uint32_t stackSize, uint8_t prio=kDefaultPrio, int8_t core=-1)
    :AudioNodeWithState(parent, tag), mStackSize(stackSize), mTaskPrio(prio), mCpuCore(core)
    {}
    void setPriority(uint8_t prio) { mTaskPrio = prio; }
    virtual bool run() override;
    virtual void stop(bool wait=true) override;
    virtual void terminate(bool wait=true) override;
};

inline bool AudioNode::plSendEvent(uint32_t type, size_t numArg, uintptr_t arg)
{
    return mPipeline.onNodeEvent(*this, type, numArg, arg);
}

inline void AudioNode::plNotifyError(int error)
{
    mPipeline.onNodeError(*this, error);
}

#endif
