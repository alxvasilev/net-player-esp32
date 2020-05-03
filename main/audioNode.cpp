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
#include "audioNode.hpp"

void AudioNodeWithTask::setState(State newState)
{
    if (newState == mState) {
        return;
    }
    ESP_LOGD(mTag, "state change %d -> %d", mState, newState);
    mState = newState;
    mEvents.clearBits(kStateStopped|kStatePaused|kStateRunning);
    mEvents.setBits(newState);
}

bool AudioNodeWithTask::createAndStartTask()
{
    mEvents.clearBits(kEvtStopRequest);
    auto ret = xTaskCreate(sTaskFunc, mTag, mStackSize, this, mTaskPrio, &mTaskId);
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
    MutexLocker locker(self->mMutex);
    self->setState(kStatePaused);
    self->nodeThreadFunc();
    self->setState(kStateStopped);
    self->mTaskId = nullptr;
    vTaskDelete(nullptr);
}

bool AudioNodeWithTask::run()
{
    auto flags = mEvents.get();
    if (flags & kStateRunning) {
        ESP_LOGW(mTag, "Node already running");
        return true;
    }
    if (flags & kStateStopped) {
        mMutex.lock(); // wait for task termination
        mMutex.unlock();
        myassert(!mTaskId);
        if (!createAndStartTask()) {
            ESP_LOGE(mTag, "Error creating task for node");
            return false;
        }
    }
    waitForState(kStatePaused);
    mCmdQueue.post(kCommandRun);
    waitForState(kStateRunning);
    return true;
}

void AudioNodeWithTask::pause(bool wait)
{
    auto state = mState;
    if (state == kStateStopped) {
        ESP_LOGW(mTag, "pause(): Node is stopped");
        return;
    } else if (state == kStatePaused) {
        return;
    } else {
        mCmdQueue.post(kCommandPause);
        if (wait) {
            waitForState(kStatePaused);
        }
    }
}

AudioNodeWithTask::State AudioNodeWithTask::waitForState(unsigned state)
{
    return (State)mEvents.waitForOneNoReset(state, -1);
}

void AudioNodeWithTask::waitForStop()
{
    waitForState(kStateStopped);
}

void AudioNodeWithTask::stop(bool wait)
{
    if (mState == kStateStopped) {
        ESP_LOGI(mTag, "stop: Already stopped");
        return;
    }
    mTerminate = true;
    mEvents.setBits(kEvtStopRequest);
    doStop();
    if (wait) {
        waitForStop();
    }
}

bool AudioNodeWithTask::dispatchCommand(Command& cmd)
{
    switch(cmd.opcode) {
    case kCommandRun:
        if (mState == kStateRunning) {
            ESP_LOGW(mTag, "kCommandRun: Already running");
        } else {
            setState(kStateRunning);
        }
        break;
    case kCommandPause:
        if (mState == kStatePaused) {
            ESP_LOGW(mTag, "kCommndPause: Already paused");
        } else {
            setState(kStatePaused);
        }
        setState(kStatePaused);
        break;
    default:
        return false;
    }
    return true;
}

void AudioNodeWithTask::processMessages()
{
    while (!mTerminate) {
        Command cmd;
        ESP_LOGD(mTag, "Waiting for command...");
        mCmdQueue.get(cmd, -1);
        dispatchCommand(cmd);
        if (mState == kStateRunning && mCmdQueue.numMessages() == 0) {
            break;
        }
    }
}
const char* AudioNode::codecTypeToStr(esp_codec_type_t type)
{
    switch (type) {
        case ESP_CODEC_TYPE_MP3: return "mp3";
        case ESP_CODEC_TYPE_AAC: return "aac";
        case ESP_CODEC_TYPE_OGG: return "ogg";
        case ESP_CODEC_TYPE_M4A: return "m4a";
        case ESP_CODEC_TYPE_FLAC: return "flac";
        case ESP_CODEC_TYPE_OPUS: return "opus";
        case ESP_CODEC_TYPE_UNKNOW: return "none";
        default: return "(unknown)";
    }
}
