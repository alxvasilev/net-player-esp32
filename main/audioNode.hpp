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
#include "streamFormat.hpp"
#include "streamRingQueue.hpp"
#include "playlist.hpp"

class AudioNode;

class IAudioPipeline {
public:
    virtual bool onNodeEvent(AudioNode& node, uint32_t type, size_t numArg, uintptr_t arg) = 0;
    virtual void onNodeError(AudioNode& node, int error) = 0;
};

class IAudioVolume;

enum StreamError: uint8_t {
    kNoError = 0,
    kErrStopped,
    kErrTimeout,
    kErrInvalidFirstChunk,
    kErrNoCodec,
    kErrDecode,
    /* In functions that call pullData() to obtain data, but need to return a StreamError code,
     * they can return this code when pullData() returns false */
    kErrUpstream
};
const char* streamErrorToString(StreamError err);


class AudioNode
{
public:
    // pipeline events
    enum { kEventAudioFormatChange, kEventNewStream, kEventLast = kEventNewStream };
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
    // always returns false, suitable for writing
    // return plNotifyError(err) in pullData()
    inline bool plNotifyError(StreamError error);
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
    virtual bool pullData(std::unique_ptr<StreamItem>& item) = 0;
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

inline bool AudioNode::plNotifyError(StreamError error)
{
    mPipeline.onNodeError(*this, error);
    return false;
}

#endif
