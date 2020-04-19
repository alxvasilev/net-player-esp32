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

static const char* TAG = "AUDIO_NODE";

void AudioNode::setState(State newState)
{
    if (newState == mState) {
        return;
    }
    ESP_LOGD(TAG, "state change %d -> %d", mState, newState);
    mState = newState;
}

void AudioNodeWithTask::setState(State newState)
{
    AudioNode::setState(newState);
    mEvents.clearBits(kStateStopped|kStatePaused|kStateRunning);
    mEvents.setBits(newState);
}

bool AudioNodeWithTask::createAndStartTask()
{
    char name[16];
    auto tagLen = strlen(mTag);
    if (tagLen > 10) {
        tagLen = 10;
    }
    memcpy(name, mTag, tagLen);
    memcpy(name + tagLen, "-task", 6);
    auto ret = xTaskCreate(sTaskFunc, name, mStackSize, this, mTaskPrio, &mTaskId);
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
        ESP_LOGW(TAG, "Node '%s' already running", mTag);
        return true;
    }
    if (flags & kStateStopped) {
        mMutex.lock(); // wait for task termination
        mMutex.unlock();
        myassert(!mTaskId);
        if (!createAndStartTask()) {
            ESP_LOGE(TAG, "Error creating task for node '%s'", mTag);
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
        ESP_LOGW(TAG, "pause(): Node is stopped");
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

void AudioNodeWithTask::stop(bool wait)
{
    auto state = mState;
    if (state == kStateStopped) {
        return;
    } else {
        mTerminate = true;
        if (wait) {
            waitForState(kStateStopped);
        }
    }
}

bool AudioNodeWithTask::dispatchCommand(Command& cmd)
{
    switch(cmd.opcode) {
    case kCommandRun:
        if (mState == kStateRunning) {
            ESP_LOGW(TAG, "kCommandRun: Already running");
        } else {
            setState(kStateRunning);
        }
        break;
    case kCommandPause:
        if (mState == kStatePaused) {
            ESP_LOGW(TAG, "kCommndPause: Already paused");
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
        ESP_LOGD(TAG, "Waiting for command...");
        mCmdQueue.get(cmd, -1);
        dispatchCommand(cmd);
        if (mState == kStateRunning && mCmdQueue.numMessages() == 0) {
            break;
        }
    }
}
