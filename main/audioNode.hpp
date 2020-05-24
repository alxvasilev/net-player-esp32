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
    kCodecOgg,
    kCodecM4a,
    kCodecFlac,
    kCodecOpus,
    kCodecWav,
    // ====
    kPlaylistM3u8,
    kPlaylistPls
};

struct StreamFormat
{
protected:
    uint8_t nChannels: 1;
public:
    uint32_t samplerate: 19;
    bool ctr: 1; // toogled with every change of the format, to signal e.g. reset of the (same) decider
protected:
    bool mReserved: 1;
    uint8_t mBits: 2;
    void zero() { memset(this, 0, sizeof(StreamFormat)); }
public:
    CodecType codec: 8;
    static uint8_t encodeBitRes(uint8_t bits) { return (bits >> 3) - 1; }
    static uint8_t decodeBitRes(uint8_t bits) { return (bits + 1) << 3; }
    StreamFormat(uint32_t sr, uint8_t bits, uint8_t channels)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        zero();
        nChannels = channels-1;
        samplerate = sr;
        mBits = encodeBitRes(bits);
    }
    StreamFormat(CodecType aCodec)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        zero();
        codec = aCodec;
    }
    StreamFormat()
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        zero();
    }
    bool operator==(StreamFormat other) const { return toCode() == other.toCode(); }
    bool operator!=(StreamFormat other) const { return toCode() != other.toCode(); }
    void reset()
    {
        bool ctrSave = ctr;
        zero();
        ctr = !ctrSave;
    }
    uint32_t toCode() const { return *reinterpret_cast<const uint32_t*>(this); }
    uint8_t bits() const { return decodeBitRes(mBits); }
    void setBits(uint8_t bits) { mBits = encodeBitRes(bits); }
    uint8_t channels() const { return nChannels + 1; }
    bool isStereo() const { return nChannels != 0; }
    void setChannels(uint8_t ch) { nChannels = ch - 1; }
    operator bool() const { return toCode() != 0; }
};

class IAudioVolume;

class AudioNode
{
public:
    enum EventType: uint16_t {
        kEventStateChange = 1,
        kEventData = 2,
        kEventLastGeneric = 15
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
    struct EventHandler
    {
        virtual bool onEvent(AudioNode* self, uint16_t type, void* buf, size_t bufSize) = 0;
    };
    const char* tag() { return mTag; }
protected:
    const char* mTag;
    Mutex mMutex;
    AudioNode* mPrev = nullptr;
    int64_t mBytePos = 0;
    void* mUserp = nullptr;
    EventHandler* mEventHandler = nullptr;
    inline void sendEvent(uint16_t type, void* buf=nullptr, int bufSize=0);
    AudioNode(const char* tag): mTag(tag) {}
public:
    virtual Type type() const = 0;
    virtual IAudioVolume* volumeInterface() { return nullptr; }
    virtual ~AudioNode() {}
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    AudioNode* prev() const { return mPrev; }
    enum StreamError: int8_t {
        kNoError = 0,
        kTimeout = -1,
        kStreamStopped = -2,
        kFormatChange = -3,
        kNeedMoreData = -4,
        kStreamFlush = - 5,
        kErrNoCodec = -6,
        kErrDecode = -7,
        kErrStreamFmt = -8
    };
    struct DataPullReq
    {
        char* buf;
        int size;
        StreamFormat fmt;
        DataPullReq(size_t aSize) { reset(aSize); }
        void reset(size_t aSize)
        {
            size = aSize;
            buf = nullptr;
        }
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamError pullData(DataPullReq& dpr, int timeout) = 0;
    virtual void confirmRead(int amount) = 0;
    static const char* codecTypeToStr(CodecType type);
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
    AudioNodeWithState(const char* tag): AudioNode(tag), mEvents(kEvtStopRequest)
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
    UBaseType_t mTaskPrio;
    Queue<Command, 4> mCmdQueue;
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
    AudioNodeWithTask(const char* tag, uint32_t stackSize, UBaseType_t prop=kDefaultPrio)
    :AudioNodeWithState(tag), mStackSize(stackSize)
    {}
    void setPriority(UBaseType_t prio) { mTaskPrio = prio; }
};

/* Interface for setting and getting volume of an audio node. If implemented,
 * flags() should have the kHasVolume bit set.
 * IMPORTANT: This interface class should have no members, because it is
 * used in multple inheritance and AudioNode is cast to IAudioVolume based
 * on the presence of kHasVolume bit in flags(). This cast will not work correctly
 * if IAudioVolume is not a pure virtual class, because the this pointers will
 * differ in the multiple inheritance
 */
class IAudioVolume
{
public:
    // volume is in percent of original.
    // 0-99% attenuates, 101-400% amplifies
    virtual uint16_t getVolume() const = 0;
    virtual void setVolume(uint16_t vol) = 0;
};

inline void AudioNode::sendEvent(uint16_t type, void *buf, int bufSize)
{
    if (!mEventHandler) {
        return;
    }
    mEventHandler->onEvent(this, type, buf, bufSize);
}
#endif
