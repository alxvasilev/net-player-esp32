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
    enum EventType: uint32_t {
        kEventStateChange = 1,
        kEventData = 2,
        kEventLastGeneric = 8
    };
    // we put here the state definitions only because the class name is shorter than AudioNodeWithTask
    enum State: uint8_t {
        kStateStopped = 1, kStatePaused = 2,
        kStateRunning = 4, kStateLast = kStateRunning
    };
    // Static registry of all possible node types.
    // Ideally each node class should have a static singleton that should be
    // initialized at runtime with an autoincremented global that is a static member
    // of this class
    enum Type: uint8_t {
        kTypeUnknown = 0,
        kTypeHttpIn,
        kTypeI2sIn,
        kTypeA2dpIn,
        kTypeDecoder,
        kTypeEncoder,
        kTypeEqualizer,
        kTypeI2sOut,
        kTypeHttpOut,
        kTypeA2dpOut
    };
    const char* tag() { return mTag; }
protected:
    static bool sHaveSpiRam;
    IAudioPipeline& mPipeline;
    const char* mTag;
    Mutex mMutex;
    AudioNode* mPrev = nullptr;
    inline void plSendEvent(uint32_t type, uintptr_t arg=0, int bufSize=0);
    inline void plNotifyError(int error);
    AudioNode(IAudioPipeline& parent, const char* tag): mPipeline(parent), mTag(tag) {}
public:
    static void detectSpiRam();
    static bool haveSpiRam() { return sHaveSpiRam; }
    static void* mallocTrySpiram(size_t internalSize, size_t spiramSize)
    {
        return sHaveSpiRam
            ? heap_caps_malloc(spiramSize, MALLOC_CAP_SPIRAM) : malloc(internalSize);
    }
    static void* mallocTrySpiram(size_t size)
    {
        return sHaveSpiRam
            ? heap_caps_malloc(size, MALLOC_CAP_SPIRAM) : malloc(size);
    }
    virtual Type type() const = 0;
    virtual IAudioVolume* volumeInterface() { return nullptr; }
    virtual ~AudioNode() {}
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    AudioNode* prev() const { return mPrev; }
    enum StreamError: int8_t {
        kNoError = 0,
        kTimeout,
        kStreamStopped,
        kStreamChanged,
        kCodecChanged,
        kTitleChanged,
        kNeedMoreData,
        kStreamFlush,
        kErrNoCodec,
        kErrDecode,
        kErrStreamFmt,
        kErrBuffer
    };
    struct DataPullReq
    {
        char* buf = nullptr;
        int size;
        union {
            StreamFormat fmt;
            CodecType codec;
        };
        DataPullReq(size_t aSize): size(aSize) {}
        void reset(size_t aSize)
        {
            size = aSize;
            buf = nullptr;
            fmt.clear();
            myassert(codec == kCodecUnknown);
        }
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamError pullData(DataPullReq& dpr) = 0;
    virtual void confirmRead(int amount) = 0;
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
    State mState = kStateStopped;
    volatile bool mTerminate = false;
    EventGroup mEvents;
    enum { kEvtStopRequest = kStateLast << 1, kEvtLast = kEvtStopRequest };
    void setState(State newState);
    virtual void doStop() { setState(kStateStopped); } // node-specific stop code goes here. Guaranteed to be called with mState != kStateStopped
    virtual void doPause() { setState(kStatePaused); }
    virtual bool doRun() { setState(kStateRunning); return true; }
public:
    AudioNodeWithState(IAudioPipeline& parent, const char* tag): AudioNode(parent, tag), mEvents(kEvtStopRequest)
    {
        mEvents.setBits(kStateStopped);
    }
    State state() const { return mState; }
    State waitForState(unsigned state);
    void pause(bool wait=true);
    void stop(bool wait=true);
    bool run();
    void waitForStop();
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
    enum: uint8_t { kCommandPause = 1, kCommandRun, kCommandLast = kCommandRun };
    enum { kDefaultPrio = 4 };
    TaskHandle_t mTaskId = NULL;
    uint32_t mStackSize;
    Queue<Command, 4> mCmdQueue;
    uint8_t mTaskPrio;
    int8_t mCpuCore;
    static void sTaskFunc(void* ctx);
    bool createAndStartTask();
    // This is the node's task function.
    // It is executed within a MutexLocker scope, and the node's state is set to
    // kStatePaused before it is called. After it returns, the node's state
    // is set to kStateStopped
    virtual void nodeThreadFunc() = 0;
    void processMessages();
    virtual bool dispatchCommand(Command& cmd);
    virtual void doPause() override { mCmdQueue.post(kCommandPause); }
    virtual bool doRun() override;
public:
    AudioNodeWithTask(IAudioPipeline& parent, const char* tag, uint32_t stackSize, uint8_t prio=kDefaultPrio, int8_t core=-1)
    :AudioNodeWithState(parent, tag), mStackSize(stackSize), mTaskPrio(prio), mCpuCore(core)
    {}
    void setPriority(uint8_t prio) { mTaskPrio = prio; }
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
