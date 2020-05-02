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
    uint8_t mBits: 3;
public:
    esp_codec_type_t codec: 8;
    static uint8_t encodeBitRes(uint8_t bits) { return (bits >> 3) - 1; }
    static uint8_t decodeBitRes(uint8_t bits) { return (bits + 1) << 3; }

    StreamFormat(uint32_t sr, uint8_t bits, uint8_t channels)
        :nChannels(channels-1), samplerate(sr),
         mBits(encodeBitRes(bits)), codec(ESP_CODEC_TYPE_UNKNOW)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
    }
    StreamFormat(esp_codec_type_t aCodec)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        memset(this, 0, sizeof(StreamFormat));
        codec = aCodec;
    }
    StreamFormat()
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        clear();
    }
    void clear() { memset(this, 0, sizeof(StreamFormat)); }
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
    enum State: uint8_t { kStateStopped = 1, kStatePaused = 2,
                          kStateRunning = 4, kStateLast = kStateRunning};
    enum EventType: uint16_t {
        kEventTypeMask = 0xff00,
        kDataEventType = 0x0100,
        kStateEventType = 0x0200,
        kUserEventTypeBase = 0x0800,
        kNoEvents = 0,
        kEventStateChange = kStateEventType | 1,
        kEventData = kDataEventType | 2
    };
    enum Flags: uint8_t { kFlagNone = 0, kFlagFixedRead = 1 };
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
    Flags mFlags;
    void setState(State newState);
    AudioNode(const char* tag, Flags flags=kFlagNone)
        : mTag(strdup(tag)), mFlags(flags) {}
public:
    virtual ~AudioNode() {}
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    Flags flags() const { return mFlags; }
    enum StreamError: int8_t {
        kNoError = 0,
        kTimeout = -1,
        kFormatChange = -2,
        kNeedMoreData = -3,
        kStreamStopped = -4,
        kErrNoCodec = -5,
        kErrDecode = -6,
    };
    struct DataPullReq
    {
        char* buf = nullptr;
        int size;
        StreamFormat fmt;
        DataPullReq(size_t aSize): size(aSize){}
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamError pullData(DataPullReq& dpr, int timeout) = 0;
    virtual void confirmRead(int amount) = 0;
};

class AudioNodeWithTask: public AudioNode
{
protected:
    struct Command
    {
        uint16_t opcode;
        void* arg;
        Command(uint8_t aCmd, void* aArg=nullptr): opcode(aCmd), arg(aArg){}
        Command(){} //no init, used for retrieving commands
    };
    enum: uint8_t { kCommandPause, kCommandRun };
    enum { kDefaultPrio = 4 };
    enum { kEventLockReleased = kStateLast << 1 };
    TaskHandle_t mTaskId = NULL;
    uint32_t mStackSize;
    UBaseType_t mTaskPrio = kDefaultPrio;
    State mState = kStateStopped;
    EventGroup mEvents;
    volatile bool mTerminate = false;
    Queue<Command, 4> mCmdQueue;
    void setState(State newState);
    State waitForState(unsigned state) { return (State)mEvents.waitForOneNoReset(state, -1); }
    bool waitForRun();
    static void sTaskFunc(void* ctx);
    bool createAndStartTask();
    // This is the node's task function.
    // It is executed within a MutexLocker scope, and the node's state is set to
    // kStatePaused before it is called. After it returns, the node's state
    // is set to kStateStopped
    virtual void nodeThreadFunc() = 0;
    virtual void doStop() = 0;
    void processMessages();
    virtual bool dispatchCommand(Command& cmd);
public:
    AudioNodeWithTask(const char* tag, uint32_t stackSize, Flags flags=kFlagNone)
    :AudioNode(tag, flags), mStackSize(stackSize)
    {
        mEvents.setBits(kStateStopped);
    }
    State state() const { return mState; }
    void setPriority(UBaseType_t prio) { mTaskPrio = prio; }
    bool run();
    void pause(bool wait=true);
    void stop(bool wait=true);
    void waitForStop();
};
#endif
