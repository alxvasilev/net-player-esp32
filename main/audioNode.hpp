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
enum CodecType: uint8_t {
    kCodecUnknown = 0,
    kCodecMp3,
    kCodecAac,
    kCodecM4a,
    kCodecOggTransport,
    kCodecOggVorbis,
    kCodecFlac,
    kCodecOggFlac,
    kCodecOpus,
    kCodecWav,
    // ====
    kPlaylistM3u8,
    kPlaylistPls
};

struct StreamFormat
{
protected:
    union {
        struct {
            uint8_t mNumChannels: 1;
            uint32_t mSampleRate: 19;
            uint8_t mBitsPerSample: 2;
            int mReserved: 2;
        };
        uint32_t mCode;
    };
public:
    operator bool() const { return mCode != 0; }
    static uint8_t encodeBps(uint8_t bits) { return (bits >> 3) - 1; }
    static uint8_t decodeBps(uint8_t bits) { return (bits + 1) << 3; }
    void clear() { mCode = 0; }
    StreamFormat(uint32_t sr, uint8_t bps, uint8_t channels): mCode(0)
    {
        setSampleRate(sr);
        setBitsPerSample(bps);
        setNumChannels(channels);
    }
    StreamFormat(): mCode(0)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "Size of StreamFormat must be 32bit");
    }
    bool operator==(StreamFormat other) const { return mCode == other.mCode; }
    bool operator!=(StreamFormat other) const { return mCode != other.mCode; }
    uint32_t asCode() const { return mCode; }
    void set(uint32_t sr, uint8_t bits, uint8_t channels) {
        setNumChannels(channels);
        setSampleRate(sr);
        setBitsPerSample(bits);
    }
    uint32_t sampleRate() const { return mSampleRate; }
    void setSampleRate(uint32_t sr) { mSampleRate = sr; }
    uint8_t bitsPerSample() const { return decodeBps(mBitsPerSample); }
    void setBitsPerSample(uint8_t bps) { mBitsPerSample = encodeBps(bps); }
    uint8_t numChannels() const { return mNumChannels + 1; }
    bool isStereo() const { return mNumChannels != 0; }
    void setNumChannels(uint8_t ch) { mNumChannels = ch - 1; }
};
const char* codecTypeToStr(CodecType type);

class AudioNode;

class IAudioPipeline {
public:
    virtual void onNodeEvent(AudioNode& node, uint32_t type, uintptr_t arg, size_t bufSize) = 0;
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
        kStreamChanged,
        kCodecChanged,
        kTitleChanged,
        kErrNoCodec,
        kErrDecode,
        kErrStreamFmt
    };
    enum { kPipeEventStreamError = 1 };

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
    inline void plSendEvent(uint32_t type, uintptr_t arg=0, int bufSize=0);
    inline void plNotifyError(int error);
    AudioNode(IAudioPipeline& parent, const char* tag): mPipeline(parent), mTag(tag) {}
public:
    virtual Type type() const = 0;
    virtual IAudioVolume* volumeInterface() { return nullptr; }
    virtual ~AudioNode() {}
    virtual void reset() {}
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    AudioNode* prev() const { return mPrev; }
    typedef uint8_t StreamId;
    struct DataPullReq
    {
        char* buf = nullptr;
        int size;
        union {
            StreamFormat fmt;
            CodecType codec;
        };
        StreamId streamId;
        DataPullReq(size_t aSize): size(aSize) {}
        void reset(size_t aSize)
        {
            size = aSize;
            buf = nullptr;
            streamId = 0;
            fmt.clear();
            myassert(codec == kCodecUnknown);
        }
        void clear() { reset(0); }
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamError pullData(DataPullReq& dpr) = 0;
    virtual void confirmRead(int amount) = 0;
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
    volatile State mState = kStateTerminated; // state is a backing store for the state in mEvents
    enum { kStateMask = (kStateLast << 1) - 1, kEvtStopRequest = kStateLast << 1, kEvtLast = kEvtStopRequest };
    /** Called internally when the state has been reached */
    void setState(State newState);
    virtual void onStopped() {}
public:
    AudioNodeWithState(IAudioPipeline& parent, const char* tag)
    : AudioNode(parent, tag), mEvents(kEvtStopRequest | kStateTerminated) {}
    State state() const { return mState; }
    State waitForState(unsigned state, int timeout=-1) { return (State)mEvents.waitForOneNoReset(state, timeout); }
    void waitForStop() { waitForState(kStateStopped); }
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

inline void AudioNode::plSendEvent(uint32_t type, uintptr_t arg, int bufSize)
{
    mPipeline.onNodeEvent(*this, type, arg, bufSize);
}

inline void AudioNode::plNotifyError(int error)
{
    mPipeline.onNodeError(*this, error);
}

#endif
