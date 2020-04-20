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
    uint8_t mChannels: 1;
    uint32_t mSamplerate: 19;
    uint8_t mBits: 3;
    uint8_t mReserved: 1;
    esp_codec_type_t mCodec: 8;
    static uint8_t encodeBitRes(uint8_t bits) { return (bits >> 3) - 1; }
    static uint8_t decodeBitRes(uint8_t bits) { return (bits + 1) << 3; }

    StreamFormat(uint32_t sr, uint8_t bits, uint8_t channels)
        :mChannels(channels), mSamplerate(sr),
         mBits(encodeBitRes(bits)), mCodec(ESP_CODEC_TYPE_UNKNOW)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
    }
    StreamFormat(esp_codec_type_t codec)
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        memset(this, 0, sizeof(StreamFormat));
        mCodec = codec;
    }
    StreamFormat()
    {
        static_assert(sizeof(StreamFormat) == sizeof(uint32_t), "");
        clear();
    }
    void clear() { memset(this, 0, sizeof(StreamFormat)); }
    uint32_t toCode() const { return *reinterpret_cast<const uint32_t*>(this); }
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
    struct EventHandler
    {
        virtual bool onEvent(AudioNode* self, uint16_t type, void* buf, size_t bufSize) = 0;
    };
    const char* tag() { return mTag; }
protected:
    char* mTag;
    Mutex mMutex;
    bool mIsWriter = false;
    AudioNode* mPrev = nullptr;
    int64_t mBytePos = 0;
    State mState = kStateStopped;
    void* mUserp = nullptr;
    EventHandler* mEventHandler = nullptr;
    EventType mSubscribedEvents = kNoEvents;
    void setState(State newState);
public:
    AudioNode(const char* tag) { mTag = strdup(tag); }
    virtual ~AudioNode() { free(mTag); }
    State state() const { return mState; }
    virtual int pullData(char* buf, size_t size, int timeout, StreamFormat& fmt) = 0;
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
    UBaseType_t mTaskPrio;
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
    void processMessages();
    virtual bool dispatchCommand(Command& cmd);
public:
    AudioNodeWithTask(const char* tag, uint32_t stackSize, UBaseType_t prio=kDefaultPrio):
        AudioNode(tag), mStackSize(stackSize), mTaskPrio(prio)
    {
        mEvents.setBits(kStateStopped);
    }
    bool run();
    void pause(bool wait=true);
    virtual void stop(bool wait=true) = 0;
    void waitForStop();
};
#endif
