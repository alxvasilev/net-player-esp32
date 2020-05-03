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
#include <audio_type_def.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "playlist.hpp"

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
    esp_codec_type_t codec: 8;
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
    StreamFormat(esp_codec_type_t aCodec)
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
    void setChannels(uint8_t ch) { nChannels = ch - 1; }
    operator bool() const { return toCode() != 0; }
};

class AudioNode
{
public:
    enum EventType: uint16_t {
        kEventTypeMask = 0xff00,
        kDataEventType = 0x0100,
        kStateEventType = 0x0200,
        kUserEventTypeBase = 0x0800,
        kNoEvents = 0,
        kEventStateChange = kStateEventType | 1,
        kEventData = kDataEventType | 2
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
    enum Type {
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
    bool mIsWriter = false;
    AudioNode* mPrev = nullptr;
    int64_t mBytePos = 0;
    void* mUserp = nullptr;
    EventHandler* mEventHandler = nullptr;
    EventType mSubscribedEvents = kNoEvents;
    AudioNode(const char* tag): mTag(tag) {}
public:
    virtual Type type() const = 0;
    virtual ~AudioNode() {}
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    enum StreamError: int8_t {
        kNoError = 0,
        kTimeout = -1,
        kStreamStopped = -2,
        kFormatChange = -3,
        kNeedMoreData = -4,
        kStreamFlush = - 5,
        kErrNoCodec = -6,
        kErrDecode = -7
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
    static const char* codecTypeToStr(esp_codec_type_t type);
};

class AudioNodeWithTask: public AudioNode
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
    enum { kEvtStopRequest = kStateLast << 1, kEvtLast = kEvtStopRequest };
    TaskHandle_t mTaskId = NULL;
    uint32_t mStackSize;
    UBaseType_t mTaskPrio;
    State mState = kStateStopped;
    EventGroup mEvents;
    volatile bool mTerminate = false;
    Queue<Command, 4> mCmdQueue;
    void setState(State newState);
    bool waitForRun();
    static void sTaskFunc(void* ctx);
    bool createAndStartTask();
    // This is the node's task function.
    // It is executed within a MutexLocker scope, and the node's state is set to
    // kStatePaused before it is called. After it returns, the node's state
    // is set to kStateStopped
    virtual void nodeThreadFunc() = 0;
    virtual void doStop() {} // node-specific stop code goes here. Guaranteed to be called with mState != kStateStopped
    void processMessages();
    virtual bool dispatchCommand(Command& cmd);
public:
    AudioNodeWithTask(const char* tag, uint32_t stackSize, UBaseType_t prop=kDefaultPrio)
    :AudioNode(tag), mStackSize(stackSize), mEvents(kEvtStopRequest)
    {
        mEvents.setBits(kStateStopped);
    }
    State state() const { return mState; }
    State waitForState(unsigned state);
    void setPriority(UBaseType_t prio) { mTaskPrio = prio; }
    bool run();
    void pause(bool wait=true);
    void stop(bool wait=true);
    void waitForStop();
};

class IAudioVolume
{
public:
    // volume is in percent of original.
    // 0-99% attenuates, 101-400% amplifies
    virtual uint8_t getVolume() const = 0;
    virtual void setVolume(uint8_t vol) = 0;
    virtual void fadeOut() = 0;
};

#endif
