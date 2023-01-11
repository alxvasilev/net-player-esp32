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
    auto ret = xTaskCreatePinnedToCore(sTaskFunc, mTag, mStackSize, this, mTaskPrio, &mTaskId,
        mCpuCore < 0 ? tskNO_AFFINITY : mCpuCore);
    if (ret == pdPASS) {
        assert(mTaskId);
        return true;
    } else {
        assert(!mTaskId);
        return false;
    }
}
void AudioNodeWithTask::sTaskFunc(void* ctx)
{
    auto self = static_cast<AudioNodeWithTask*>(ctx);
    self->setState(kStateStopped);
    self->nodeThreadFunc();
    {
        MutexLocker locker(self->mMutex);
        self->mTaskId = nullptr;
        self->setState(kStateTerminated);
    }
    vTaskDelete(nullptr);
}

bool AudioNodeWithTask::run()
{
    {
        MutexLocker locker(mMutex);
        auto currState = state();
        if (currState == kStateRunning) {
            ESP_LOGI(mTag, "run: Already running");
            return true;
        }
        else if (currState == kStateTerminated) {
            myassert(!mTaskId);
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
            ESP_LOGI(mTag, "stop: Already %s", stateToStr(currState));
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
            ESP_LOGI(mTag, "terminate: Already terminated");
            return;
        }
        mTerminate = true;
        onStopRequest();
        mEvents.setBits(kEvtStopRequest);
    }
    if (wait) {
        waitForTerminate();
    }
}

bool AudioNodeWithTask::dispatchCommand(Command& cmd)
{
    // State mutex not locked
    auto currState = state();
    switch(cmd.opcode) {
    case kCommandRun:
        if (currState == kStateRunning) {
            ESP_LOGI(mTag, "kCommandRun: Already running");
        } else {
            setState(kStateRunning);
        }
        break;
    case kCommandStop:
        if (currState == kStateStopped || currState == kStateTerminated) {
            ESP_LOGI(mTag, "kCommandStop: Already %s", stateToStr(currState));
        } else {
            setState(kStateStopped);
        }
        break;
    default:
        return false;
    }
    return false;
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
const char* codecTypeToStr(CodecType type)
{
    switch (type) {
        case kCodecMp3: return "mp3";
        case kCodecAac: return "aac";
        case kCodecOggTransport: return "ogg";
        case kCodecM4a: return "m4a";
        case kCodecFlac: return "flac";
        case kCodecOpus: return "opus";
        case kCodecOggFlac: return "ogg/flac";
        case kCodecOggVorbis: return "ogg/vobris";
        case kCodecUnknown: return "none";
        default: return "(unknown)";
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
const char* AudioNode::streamEventToStr(StreamError evt) {
    switch (evt) {
        case kTimeout: return "kTimeout";
        case kStreamStopped: return "kStreamStopped";
        case kStreamEnd: return "kStreamEnd";
        case kStreamChanged: return "kStreamChanged";
        case kCodecChanged: return "kCodecChanged";
        case kTitleChanged: return "kTitleChanged";
        case kErrNoCodec: return "kErrNoCodec";
        case kErrDecode: return "kErrDecode";
        case kErrStreamFmt: return "kErrStreamFmt";
        case kNoError: return "kNoError";
        default: return "(invalid)";
    }
}
