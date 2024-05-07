#ifndef AUDIO_NODE_HPP
#define AUDIO_NODE_HPP

#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <eventGroup.hpp>
#include "streamPackets.hpp"
#include "queue.hpp"
#include "utils.hpp"

class AudioNode;

class IAudioPipeline {
public:
    virtual bool onNodeEvent(AudioNode& node, uint32_t type, size_t numArg, uintptr_t arg) = 0;
    virtual void onNodeError(AudioNode& node, int error, uintptr_t arg) = 0;
};

class IAudioVolume;

class AudioNode
{
public:
    // events sent to the GUI via plSendEvent()
    enum { kEventStreamError = 1, kEventAudioFormatChange,
           kEventNewStream, kEventStreamEnd, kEventTrackInfo,
           kEventConnecting, kEventConnected,
           kEventPlaying, kEventRecording, kEventBufState,
           kEventLast = kEventBufState };
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
    inline void plSendError(int error, uintptr_t arg);
    AudioNode(IAudioPipeline& parent, const char* tag): mPipeline(parent), mTag(tag) {}
public:
    virtual Type type() const = 0;
    virtual IAudioVolume* volumeInterface() { return nullptr; }
    virtual ~AudioNode() {}
    virtual void reset() {}
    virtual bool waitForPrefill() { return true; }
    virtual DataPacket* peekData(bool& preceded) { return nullptr; }
    virtual StreamPacket* peek() { return nullptr; }
    void linkToPrev(AudioNode* prev) { mPrev = prev; }
    AudioNode* prev() const { return mPrev; }
    typedef uint8_t StreamId;
    struct PacketResult
    {
        StreamPacket::unique_ptr packet;
        StreamId streamId;
        DataPacket& dataPacket() {
            myassert(packet && packet->type == kEvtData);
            return *(DataPacket*)packet.get();
        }
        GenericEvent& genericEvent() {
            myassert(packet->type == kEvtStreamChanged || packet->type == kEvtStreamEnd);
            return *(GenericEvent*)packet.get();
        }
        void clear()
        {
            packet.reset();
            streamId = 0;
        }
        StreamEvent set(StreamPacket* pkt) {
            packet.reset(pkt);
            return packet->type;
        }
        StreamEvent set(StreamPacket::unique_ptr& pkt) {
            packet.reset(pkt.release());
            return packet->type;
        }
    };

    // Upon return, buf is set to the internal buffer containing the data, and size is updated to the available data
    // for reading from it. Once the caller reads the amount it needs, it must call
    // confirmRead() with the actual amount read.
    virtual StreamEvent pullData(PacketResult& pr) = 0;
    /** Reads exactly the specified amount of bytes to buf, or discards them
     *  if \c buf is null */
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

inline void AudioNode::plSendError(int error, uintptr_t arg)
{
    mPipeline.onNodeError(*this, error, arg);
}

#endif
