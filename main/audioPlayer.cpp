#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include <esp_system.h>
#include "utils.hpp"
#include "audioPlayer.hpp"
#include "httpNode.hpp"
#include "i2sSinkNode.hpp"
#include "decoderNode.hpp"
#include "equalizerNode.hpp"
#include "a2dpInputNode.hpp"

constexpr int AudioPlayer::mEqualizerDefaultGainTable[] = {
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6,
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6
};


const uint16_t AudioPlayer::equalizerFreqs[10] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

#define LOCK_PLAYER() MutexLocker locker(mutex)

void AudioPlayer::createOutputA2dp()
{
    /*
    assert(!mStreamOut);
    ESP_LOGI(TAG, "Creating a2dp output source");
    ESP_LOGI(TAG, "\tCreating Bluetooth service");
    bluetooth_service_cfg_t cfg;
    cfg.device_name = "ESP-ADF-SOURCE";
    cfg.mode = BLUETOOTH_A2DP_SOURCE;
    cfg.remote_name = "DL-LINK";
    ESP_ERROR_CHECK(bluetooth_service_start(&cfg));
    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    mStreamOut = bluetooth_service_create_stream();
    assert(mStreamOut);
    const uint8_t* addr = esp_bt_dev_get_address();
    char strAddr[13];
    binToHex(addr, 6, strAddr);
    ESP_LOGW("BT", "Own BT MAC: '%s'", strAddr);
//  Move this to execute only once
    */
}

AudioPlayer::AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, bool useEq)
:mFlags(useEq ? kFlagUseEqualizer : (Flags)0)
{
    createPipeline(inType, outType);
}
void AudioPlayer::createPipeline(AudioNode::Type inType, AudioNode::Type outType)
{
    ESP_LOGI(TAG, "Creating audio pipeline");
    AudioNode* pcmSource = nullptr;
    switch(inType) {
    case AudioNode::kTypeHttpIn:
        mStreamIn.reset(new HttpNode(kHttpBufSize));
        mDecoder.reset(new DecoderNode);
        mDecoder->linkToPrev(mStreamIn.get());
        pcmSource = mDecoder.get();
        break;
    case AudioNode::kTypeA2dpIn:
        mStreamIn.reset(new A2dpInputNode("NetPlayer"));
        mDecoder.reset();
        pcmSource = mStreamIn.get();
        break;
    default:
        myassert(false);
    }
    if (mFlags & kFlagUseEqualizer) {
        mEqualizer.reset(new EqualizerNode);
        mEqualizer->linkToPrev(pcmSource);
        pcmSource = mEqualizer.get();
    }
    switch(outType) {
    case AudioNode::kTypeI2sOut:
        mStreamOut.reset(new I2sOutputNode(0xff, nullptr));
        break;
    /*
    case kOutputA2dp:
        createOutputA2dp();
        break;
    */
    default:
        myassert(false);
    }
    mStreamOut->linkToPrev(pcmSource);
    detectVolumeNode();
}

void AudioPlayer::detectVolumeNode() {
    for (AudioNode* node = mStreamOut.get(); node; node = node->prev()) {
        mVolumeInterface = node->volumeInterface();
        if (mVolumeInterface) {
            ESP_LOGW(TAG, "Volume node found: '%s'", node->tag());
            return;
        }
    }
    ESP_LOGE(TAG, "No node with volume interface found, volume control will be unavailable");
}

void AudioPlayer::destroyPipeline()
{
    if (!mStreamIn) {
        return;
    }
    stop();
    mStreamIn.reset();
    mDecoder.reset();
    mEqualizer.reset();
    mStreamOut.reset();
}

void AudioPlayer::playUrl(const char* url)
{
    LOCK_PLAYER();
    assert(mStreamIn && mStreamIn->type() == AudioNode::kTypeHttpIn);
    auto& http = *static_cast<HttpNode*>(mStreamIn.get());
    http.setUrl(url);
    if (isStopped()) {
        play();
    }
}

bool AudioPlayer::isStopped() const
{
    if (!mStreamIn || !mStreamOut) {
        return true;
    }
    return mStreamIn->state() == AudioNode::kStateStopped ||
           mStreamOut->state() == AudioNode::kStateStopped;
}
bool AudioPlayer::isPaused() const
{
    return mStreamIn->state() <= AudioNode::kStatePaused ||
           mStreamOut->state() <= AudioNode::kStatePaused;
}

bool AudioPlayer::isPlaying() const
{
    return mStreamIn->state() == AudioNode::kStateRunning ||
           mStreamOut->state() == AudioNode::kStateRunning;
}

void AudioPlayer::play()
{
    LOCK_PLAYER();
    mStreamIn->run();
    mStreamOut->run();
}

void AudioPlayer::pause()
{
    LOCK_PLAYER();
    mStreamIn->pause();
    mStreamOut->pause();
    mStreamIn->waitForState(AudioNodeWithTask::kStatePaused);
    mStreamOut->waitForState(AudioNodeWithTask::kStatePaused);
}

void AudioPlayer::resume()
{
    play();
}

void AudioPlayer::stop()
{
   LOCK_PLAYER();
   mStreamIn->stop(false);
   mStreamOut->stop(false);
   mStreamIn->waitForStop();
   mStreamOut->waitForStop();
}

bool AudioPlayer::volumeSet(uint16_t vol)
{
    LOCK_PLAYER();
    if (mVolumeInterface) {
        mVolumeInterface->setVolume(vol);
        return true;
    }
    return false;
}

int AudioPlayer::volumeGet()
{
    LOCK_PLAYER();
    if (mVolumeInterface) {
        return mVolumeInterface->getVolume();
    }
    return -1;
}

uint16_t AudioPlayer::volumeChange(int step)
{
    LOCK_PLAYER();
    auto currVol = volumeGet();
    if (currVol < 0) {
        return currVol;
    }
    double newVol = currVol + step;
    if (newVol < 0) {
        newVol = 0;
    } else if (newVol > 255) {
        newVol = 255;
    }
    if (fabs(newVol - currVol) > 0.01) {
        if (!volumeSet(newVol)) {
            return -1;
        }
    }
    return newVol;
}

bool AudioPlayer::equalizerSetBand(int band, int8_t dbGain)
{
    LOCK_PLAYER();
    if (!mEqualizer) {
        return false;
    }
    mEqualizer->setBandGain(band, dbGain);
    return true;
}

bool AudioPlayer::equalizerSetGainsBulk(char* str, size_t len)
{
    LOCK_PLAYER();
    KeyValParser vals(str, len);
    vals.parse(';', '=', KeyValParser::kTrimSpaces);
    bool ok = true;
    for (const auto& kv: vals.keyVals()) {
        int band = kv.key.toInt(0xff);
        if (band < 0 || band > 9) {
            ok = false;
        }
        int gain = kv.val.toInt(0xff);
        if (gain == 0xff) {
            ok = false;
            continue;
        }
        if (gain < -50) {
            gain = -50;
        } else if (gain > 50) {
            gain = 50;
        }
        mEqualizer->setBandGain(band, gain);
    }
    return ok;
}

const float* AudioPlayer::equalizerDumpGains()
{
    if (!mEqualizer) {
        return nullptr;
    }
    return mEqualizer->allGains();
}

AudioPlayer::~AudioPlayer()
{
    destroyPipeline();
}
