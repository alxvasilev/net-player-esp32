#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include "equalizer.h"
#include "esp_peripherals.h"
#include "bluetooth_service.h"
#include <esp_system.h>
#include <esp_bt_device.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <a2dp_stream.h>
#include "utils.hpp"
#include "audioPlayer.hpp"
#include "httpNode.hpp"
#include "i2sSinkNode.hpp"
#include "decoderNode.hpp"

constexpr int AudioPlayer::mEqualizerDefaultGainTable[] = {
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6,
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6
};


const uint16_t AudioPlayer::equalizerFreqs[10] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

#define LOCK_PLAYER() MutexLocker locker(mutex)

void AudioPlayer::createInputA2dp()
{
    assert(!mStreamIn);
    static constexpr const char* BT = "BT";
    ESP_LOGI(BT, "Init Bluetooth");
    ESP_LOGW(BT, "Free memory before releasing BLE memory: %d", xPortGetFreeHeapSize());
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    ESP_LOGW(BT, "Free memory after releasing BLE memory: %d", xPortGetFreeHeapSize());

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGW(BT, "Free memory after enable bluedroid: %d", xPortGetFreeHeapSize());

    esp_bt_dev_set_device_name("NetPlayer");

    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
    ESP_LOGI(BT, "Get Bluetooth stream");
    a2dp_stream_config_t cfg = {
        .type = AUDIO_STREAM_READER,
        .user_callback = {
            .user_a2d_cb = [](esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
                ESP_LOGI(BT, "A2DP stream event %d", event);
            },
            .user_a2d_sink_data_cb = [](const uint8_t *buf, uint32_t len) {
                //static uint8_t ctr = 0;
                //gpio_set_level(kPinLed, (++ctr) & 1);
            },
            nullptr
        }
    };
/*
    Commented out because this doesnt compile anymore
    mStreamIn = a2dp_stream_init(&cfg);
    assert(mStreamIn);
    audio_element_set_event_callback(mStreamIn, inputFormatEventCb, this);
*/
    ESP_LOGI(BT, "Create and start Bluetooth peripheral");
    auto bt_periph = bt_create_periph();
    ESP_ERROR_CHECK(esp_periph_start(mPeriphSet, bt_periph));
}

void AudioPlayer::createOutputA2dp()
{
    assert(!mStreamOut);
    ESP_LOGI(TAG, "Creating a2dp output source");
    ESP_LOGI(TAG, "\tCreating Bluetooth service");
    bluetooth_service_cfg_t cfg;
    cfg.device_name = "ESP-ADF-SOURCE";
    cfg.mode = BLUETOOTH_A2DP_SOURCE;
    cfg.remote_name = "DL-LINK";
    ESP_ERROR_CHECK(bluetooth_service_start(&cfg));
/* Doesn't compile anymore
    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    mStreamOut = bluetooth_service_create_stream();
    assert(mStreamOut);
*/
    const uint8_t* addr = esp_bt_dev_get_address();
    char strAddr[13];
    binToHex(addr, 6, strAddr);
    ESP_LOGW("BT", "Own BT MAC: '%s'", strAddr);
//  Move this to execute only once
    ESP_LOGI(TAG, "\tCreating and starting Bluetooth peripheral");
    esp_periph_handle_t btPeriph = bluetooth_service_create_periph();
    assert(btPeriph);
    ESP_ERROR_CHECK(esp_periph_start(mPeriphSet, btPeriph));
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
        createInputA2dp();
        mDecoder.reset();
        pcmSource = mStreamIn.get();
        break;
    default:
        myassert(false);
    }
    if (mFlags & kFlagUseEqualizer) {
/*
        mEqualizer.reset(new EqualizerNode);
        mEqualizer->linkToPrev(pcmSource);
        pcmSource = mEqualizer.get();
*/
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
    if (mDecoder) {
        static_cast<DecoderNode*>(mDecoder.get())->setVolume(vol);
        return true;
    }
    return false;
}

int AudioPlayer::volumeGet()
{
    LOCK_PLAYER();
    if (mDecoder) {
        return static_cast<DecoderNode*>(mDecoder.get())->getVolume();
    }
    return -1;
}

int AudioPlayer::volumeChange(int step)
{
    LOCK_PLAYER();
    int currVol = volumeGet();
    if (currVol < 0) {
        return currVol;
    }
    int newVol = currVol + step;
    if (newVol < 0) {
        newVol = 0;
    } else if (newVol > 255) {
        newVol = 255;
    }
    if (newVol != currVol) {
        if (!volumeSet(newVol)) {
            return -1;
        }
    }
    return newVol;
}

bool AudioPlayer::equalizerSetBand(int band, int level)
{
/*
    LOCK_PLAYER();
    if (!mEqualizer) {
        return false;
    }
    return equalizer_set_gain_info(mEqualizer, band, level, 1) == ESP_OK;
*/
    return true;
}

bool AudioPlayer::equalizerSetGainsBulk(char* str, size_t len)
{
/*
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
        if (gain < 50 || gain > 50) {
            ok = false;
        }
        ok &= (equalizer_set_gain_info(mEqualizer, band, gain, 1) != ESP_OK);
    }
    return ok;
*/
    return true;
}

int* AudioPlayer::equalizerDumpGains()
{
/*
    equalizer_cfg_t cfg = DEFAULT_EQUALIZER_CONFIG();
    return cfg.set_gain;
*/
    return nullptr;
}

AudioPlayer::~AudioPlayer()
{
    destroyPipeline();
}
