#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/projdefs.h>
#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "playlist.hpp"
#include "audioNode.hpp"

void AudioNodeWithState::setState(State newState)
{
    MutexLocker locker(mMutex);
    auto prevState = state();
    if (newState == prevState) {
        return;
    }
    ESP_LOGI(mTag, "State change: %s -> %s", stateToStr(prevState), stateToStr(newState));
    EventBits_t clearStopReq = (newState == kStateRunning) ? kEvtStopRequest : 0;
    mEvents.clearBits(kStateTerminated|kStateStopped|kStateRunning|clearStopReq);
    mEvents.setBits(newState);
    mState = newState;

    if (mState == kStateStopped || ((mState == kStateTerminated) && (prevState == kStateRunning))) {
        onStopped();
    }
}
void AudioNodeWithState::terminate(bool wait)
{
    setState(kStateTerminated);
}
void AudioNodeWithState::stop(bool wait)
{
    setState(kStateStopped);
}
bool AudioNodeWithState::run()
{
    setState(kStateRunning);
    return true;
}
bool AudioNodeWithTask::createAndStartTask()
{
    mEvents.clearBits(kEvtStopRequest);
    mTerminate = false;
    auto ret = mTask.createTask(mTag, mStackInPsram, mStackSize, mCpuCore < 0 ? tskNO_AFFINITY : mCpuCore, mTaskPrio,
        this, AudioNodeWithTask::sTaskFunc);
    if (!ret) {
        ESP_LOGE(mTag, "Out of memory allocating task memory");
        return false;
    }
    return true;
}
void AudioNodeWithTask::sTaskFunc(void* ctx)
{
    auto self = static_cast<AudioNodeWithTask*>(ctx);
    self->setState(kStateStopped);
    self->nodeThreadFunc();
    self->setState(kStateTerminated);
}

bool AudioNodeWithTask::run()
{
    {
        MutexLocker locker(mMutex);
        auto currState = state();
        if (currState == kStateRunning) {
            ESP_LOGD(mTag, "run: Already running");
            return true;
        }
        else if (currState == kStateTerminated) {
            //myassert(!mTaskId);
            if (!createAndStartTask()) {
                ESP_LOGE(mTag, "Error creating task for node");
                return false;
            }
        }
    }
    mCmdQueue.post(kCommandRun);
    return true;
}

void AudioNodeWithTask::stop(bool wait)
{
    {
        MutexLocker locker(mMutex);
        auto currState = state();
        if (currState == kStateTerminated || currState == kStateStopped) {
            ESP_LOGD(mTag, "stop: Already %s", stateToStr(currState));
            return;
        }
        onStopRequest();
        mEvents.setBits(kEvtStopRequest);
    }
    mCmdQueue.post(kCommandStop);
    if (wait) {
        waitForState(kStateStopped);
    }
}

void AudioNodeWithTask::terminate(bool wait)
{
    {
        MutexLocker locker(mMutex);
        if (state() == kStateTerminated) {
            ESP_LOGD(mTag, "terminate: Already terminated");
            return;
        }
        mTerminate = true;
        onStopRequest();
        mEvents.setBits(kEvtStopRequest);
    }
    mCmdQueue.post(kCommandTerminate);
    if (wait) {
        waitForState(kStateTerminated);
    }
}

bool AudioNodeWithTask::dispatchCommand(Command& cmd)
{
    // State mutex not locked
    auto currState = state();
    switch(cmd.opcode) {
    case kCommandRun:
        if (currState == kStateRunning) {
            ESP_LOGD(mTag, "kCommandRun: Already running");
        } else {
            setState(kStateRunning);
        }
        return true;
    case kCommandStop:
        if (currState == kStateStopped || currState == kStateTerminated) {
            ESP_LOGD(mTag, "kCommandStop: Already %s", stateToStr(currState));
        } else {
            setState(kStateStopped);
        }
        return true;
    case kCommandTerminate: // just need to wake up the event loop
        return true;
    default:
        return false;
    }
    return false; // never reached
}

void AudioNodeWithTask::processMessages()
{
    while (!mTerminate) {
        Command cmd;
        ESP_LOGD(mTag, "Waiting for command...");
        mCmdQueue.get(cmd, -1);
        {
            dispatchCommand(cmd);
            if (state() == kStateRunning && mCmdQueue.numMessages() == 0) {
                break;
            }
        }
    }
}
const char* AudioNodeWithState::stateToStr(State state)
{
    switch(state) {
        case kStateRunning: return "running";
        case kStateStopped: return "stopped";
        case kStateTerminated: return "terminated";
        default: return "(invalid)";
    }
}
