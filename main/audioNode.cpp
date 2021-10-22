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

bool AudioNode::sHaveSpiRam = false;
void AudioNode::detectSpiRam()
{
    // Detect SPI RAM presence
    auto buf = heap_caps_malloc(4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf)
    {
        free(buf);
        sHaveSpiRam = true;
        ESP_LOGI("", "SPI RAM available");
    }
    else
    {
        ESP_LOGI("", "SPI RAM NOT available");
    }
}

void AudioNodeWithState::setState(State newState)
{
    if (newState == mState) {
        return;
    }
    ESP_LOGI(mTag, "state change %d -> %d", mState, newState);
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

bool AudioNodeWithState::run()
{
    if (mEvents.get() & kStateRunning) {
        ESP_LOGW(mTag, "Node already running");
        return true;
    }
    if (!doRun()) {
        return false;
    }
    waitForState(kStateRunning);
    return true;
}

bool AudioNodeWithTask::doRun()
{
    if (mEvents.get() & kStateStopped) {
        mMutex.lock(); // wait for task cleanup
        mMutex.unlock();
        myassert(!mTaskId);
        if (!createAndStartTask()) {
            ESP_LOGE(mTag, "Error creating task for node");
            return false;
        }
    }
    waitForState(kStatePaused);
    mCmdQueue.post(kCommandRun);
    return true;
}

void AudioNodeWithState::pause(bool wait)
{
    auto state = mState;
    if (state == kStateStopped) {
        ESP_LOGW(mTag, "pause(): Node is stopped");
        return;
    } else if (state == kStatePaused) {
        return;
    } else {
        doPause();
        if (wait) {
            waitForState(kStatePaused);
        }
    }
}

AudioNodeWithState::State AudioNodeWithState::waitForState(unsigned state)
{
    return (State)mEvents.waitForOneNoReset(state, -1);
}

void AudioNodeWithState::waitForStop()
{
    waitForState(kStateStopped);
}

void AudioNodeWithState::stop(bool wait)
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
const char* StreamFormat::codecTypeToStr(CodecType type)
{
    switch (type) {
        case kCodecMp3: return "mp3";
        case kCodecAac: return "aac";
        case kCodecOgg: return "ogg";
        case kCodecM4a: return "m4a";
        case kCodecFlac: return "flac";
        case kCodecOpus: return "opus";
        case kCodecUnknown: return "none";
        default: return "(unknown)";
    }
}
