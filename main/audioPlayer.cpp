#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include "audio_common.h"
#include "http_stream.h"
#include "equalizer.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
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

constexpr int AudioPlayer::mEqualizerDefaultGainTable[] = {
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6,
    8, 8, 7, 4, 2, 0, 0, 2, 4, 6
};

void AudioPlayer::createInputHttp()
{
    assert(!mStreamIn);
    ESP_LOGI("HTTP", "Create http stream reader");
    http_stream_cfg_t cfg = myHTTP_STREAM_CFG_DEFAULT;
    //  http_cfg.multi_out_num = 1;
    cfg.enable_playlist_parser = 1;
    cfg.event_handle = httpEventHandler;
//  cfg.out_rb_size = 30 * 1024;
    mInputType = kInputHttp;
    mStreamIn = http_stream_init(&cfg);
    assert(mStreamIn);
}

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

    mStreamIn = a2dp_stream_init(&cfg);
    assert(mStreamIn);
    mInputType = kInputA2dp;

    ESP_LOGI(BT, "Create and start Bluetooth peripheral");
    auto bt_periph = bt_create_periph();
    ESP_ERROR_CHECK(esp_periph_start(mPeriphSet, bt_periph));
}

void AudioPlayer::createOutputI2s()
{
    assert(!mStreamOut);
    ESP_LOGI(TAG, "Creating i2s output to write data to codec chip");
    i2s_stream_cfg_t cfg = myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
    cfg.type = AUDIO_STREAM_WRITER;
    cfg.use_alc = 1;
    mStreamOut = i2s_stream_init(&cfg);
    assert(mStreamOut);
    mOutputType = kOutputI2s;
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

    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    mStreamOut = bluetooth_service_create_stream();
    assert(mStreamOut);

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

void AudioPlayer::createOutputElement(OutputType type)
{
    assert(mOutputType == kOutputNone);
    assert(!mStreamOut);
    switch(type) {
    case kOutputI2s: {
        createOutputI2s();
        break;
    }
    case kOutputA2dp: {
        createOutputA2dp();
        break;
    }
    default:
        assert(false);
    }
    assert(mOutputType != kOutputNone);
}

void AudioPlayer::createDecoderByType(CodecType type)
{
    assert(!mDecoder);
    mDecoderType = type;
    switch (type) {
    case kCodecMp3: {
        mp3_decoder_cfg_t cfg = DEFAULT_MP3_DECODER_CONFIG();
        cfg.task_core = 1;
        mDecoder = mp3_decoder_init(&cfg);
        break;
    }
    case kCodecAac: {
        aac_decoder_cfg_t cfg = DEFAULT_AAC_DECODER_CONFIG();
        mDecoder = aac_decoder_init(&cfg);
        break;
    }
    default:
        mDecoder = nullptr;
        mDecoderType = kCodecNone;
        assert(false);
        return;
    }
}
esp_err_t AudioPlayer::inputFormatEventCb(audio_element_handle_t el,
    audio_event_iface_msg_t* msg, void* ctx)
{
    assert(ctx);
    auto self = static_cast<AudioPlayer*>(ctx);
    ESP_LOGI(TAG, "=======================inputFormatEventCb");
    if (msg->cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t info;
        memset(&info, 0, sizeof(info));
        audio_element_getinfo(el, &info);

        ESP_LOGI(TAG, "Received music info from samplerate source:\n"
            "samplerate: %d, bits: %d, ch: %d, bps: %d, codec: %d",
            info.sample_rates, info.bits, info.channels, info.bps, info.codec_fmt);
        audio_element_setinfo(self->mStreamOut, &info);
        if (self->mOutputType == kOutputI2s) {
            i2s_stream_set_clk(self->mStreamOut, info.sample_rates, info.bits, info.channels);
        }
    }
    return ESP_OK;
}

void AudioPlayer::createEqualizer()
{
    equalizer_cfg_t cfg = DEFAULT_EQUALIZER_CONFIG();
    // The size of gain array should be the multiplication of NUMBER_BAND
    // and number channels of audio stream data. The minimum of gain is -13 dB.
    // TODO: Load equalizer from nvs
    memcpy(cfg.set_gain, mEqualizerDefaultGainTable, sizeof(mEqualizerDefaultGainTable));
    mEqualizer = equalizer_init(&cfg);
}

void AudioPlayer::destroyEqualizer()
{
    if (!mEqualizer) {
        return;
    }
    assert(mState != kStatePlaying);
    ESP_ERROR_CHECK(audio_pipeline_unregister(mPipeline, mEqualizer));
    ESP_ERROR_CHECK(audio_element_deinit(mEqualizer));
    mEqualizer = nullptr;
}

AudioPlayer::AudioPlayer(OutputType outType, bool useEq)
:mFlags(useEq ? kFlagUseEqualizer : (Flags)0)
{
    createOutputSide(outType);
}

void AudioPlayer::createOutputSide(OutputType outType)
{
    ESP_LOGI(TAG, "Create audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    mPipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(mPipeline);
    if (mFlags & kFlagUseEqualizer) {
        createEqualizer();
    } else {
        mEqualizer = nullptr;
    }
    createOutputElement(outType);
}

void AudioPlayer::setSourceUrl(const char* url, CodecType codecType)
{
    if (mInputType == kInputNone) {
        createInputSide(kInputHttp, codecType);
        linkPipeline();
    } else {
        assert(mInputType == kInputHttp);
    }
    ESP_LOGI(TAG, "Set http stream uri to '%s', format: %s", url, codecTypeToStr(codecType));
    ESP_LOGI(TAG, "setSourceUrl: current state is %d", mState);
    bool wasPlaying = (mState == kStatePlaying);
    if (wasPlaying) {
        stop();
    }
    ESP_ERROR_CHECK(audio_element_set_uri(mStreamIn, url));
    if (wasPlaying) {
        play();
    }
}

void AudioPlayer::createInputSide(InputType inType, CodecType codecType)
{
    assert(mState == kStateStopped);
    assert(mInputType == kInputNone);
    assert(mDecoderType == kCodecNone);
    ESP_LOGD(TAG, "createInputSide");
    if (inType == kInputHttp) {
        createInputHttp();
        createDecoderByType(codecType);
        assert(mDecoderType == codecType);
        audio_element_set_event_callback(mDecoder, inputFormatEventCb, this);
    } else if (inType == kInputA2dp) {
        createInputA2dp();
        mDecoder = nullptr;
        audio_element_set_event_callback(mStreamIn, inputFormatEventCb, this);
    }
    assert(mInputType == inType);
}

void AudioPlayer::linkPipeline()
{
    ESP_LOGI(TAG, "Registering and linking pipeline elements");
    std::vector<const char*> order;
    order.reserve(4);
    order.push_back("in");
    ESP_ERROR_CHECK(audio_pipeline_register(mPipeline, mStreamIn, order.back()));
    if (mDecoder) {
        order.push_back("dec");
        ESP_ERROR_CHECK(audio_pipeline_register(mPipeline, mDecoder, order.back()));
        mSamplerateSource = mDecoder;
    } else {
        mSamplerateSource = mStreamIn;
    }
    if (mEqualizer) {
        order.push_back("eq");
        ESP_ERROR_CHECK(audio_pipeline_register(mPipeline, mEqualizer, order.back()));
    }
    order.push_back("out");
    ESP_ERROR_CHECK(audio_pipeline_register(mPipeline, mStreamOut, order.back()));

    ESP_ERROR_CHECK(audio_pipeline_link(mPipeline, order.data(), order.size()));
    ESP_LOGI(TAG, "Connecting event listener");
}

void AudioPlayer::play()
{
    if (mState == kStatePlaying) {
        ESP_LOGW(TAG, "AudioPlayer::play: already playing");
    }
    // Listening event from all elements of audio pipeline
    // NOTE: This must be re-applied after pipeline change
    ESP_LOGI(TAG, "Starting pipeline");
    audio_pipeline_run(mPipeline);
/*
    if (mState == kStatePaused) {
        resume();
    } else {
*/
        mState = kStatePlaying;
//  }
}

void AudioPlayer::pause()
{
    assert(mState = kStatePlaying);
    ESP_ERROR_CHECK(audio_pipeline_pause(mPipeline));
    mState = kStatePaused;
}

void AudioPlayer::resume()
{
    assert(mState == kStatePaused);
    ESP_ERROR_CHECK(audio_pipeline_resume(mPipeline));
    mState = kStatePlaying;
}

void AudioPlayer::stop()
{
    if (mState == kStateStopped) {
        ESP_LOGW(TAG, "stop: already stopped");
        return;
    }
    ESP_LOGI(TAG, "Stopping pipeline");
    ESP_ERROR_CHECK(audio_pipeline_stop(mPipeline));
    ESP_ERROR_CHECK(audio_pipeline_wait_for_stop(mPipeline));
    mState = kStateStopped;
    ESP_LOGI(TAG, "Pipeline stopped");
}

void AudioPlayer::destroyInputSide()
{
    assert(mState == kStateStopped);
    assert(mInputType != kInputNone);
    assert(mStreamIn);
    ESP_ERROR_CHECK(audio_pipeline_breakup_elements(mPipeline, nullptr));

    ESP_ERROR_CHECK(audio_pipeline_unregister(mPipeline, mStreamIn));
    ESP_ERROR_CHECK(audio_element_deinit(mStreamIn));
    mStreamIn = nullptr;
    mInputType = kInputNone;
    if (mDecoder) {
        ESP_ERROR_CHECK(audio_pipeline_unregister(mPipeline, mDecoder));
        ESP_ERROR_CHECK(audio_element_deinit(mDecoder));
        mDecoderType = kCodecNone;
    }
}

void AudioPlayer::destroyOutputSide()
{
    assert(mState == kStateStopped);
    assert(mOutputType != kOutputNone);
    assert(mStreamOut);
    ESP_ERROR_CHECK(audio_pipeline_breakup_elements(mPipeline, nullptr));

    if (mEqualizer) {
        destroyEqualizer();
    }
    ESP_ERROR_CHECK(audio_pipeline_unregister(mPipeline, mStreamOut));
    ESP_ERROR_CHECK(audio_element_deinit(mStreamOut));
    mStreamOut = nullptr;
    mOutputType = kOutputNone;
}
bool AudioPlayer::setVolume(int vol)
{
    if (mOutputType == kOutputI2s) {
        ESP_ERROR_CHECK(i2s_alc_volume_set(mStreamOut, vol));
        return true;
    }
    return false;
}
int AudioPlayer::getVolume()
{
    if (mOutputType == kOutputI2s) {
        int vol;
        ESP_ERROR_CHECK(i2s_alc_volume_get(mStreamOut, &vol));
        return vol;
    } else {
        return -1;
    }
}
int AudioPlayer::changeVolume(int step)
{
    int currVol = getVolume();
    if (currVol < 0) {
        return currVol;
    }
    int newVol = currVol + step;
    if (newVol < 0) {
        newVol = 0;
    } else if (newVol > 100) {
        newVol = 100;
    }
    if (newVol != currVol) {
        if (!setVolume(newVol)) {
            return -1;
        }
    }
    return newVol;
}

AudioPlayer::~AudioPlayer()
{
    assert(mState == kStateStopped);
    ESP_ERROR_CHECK(audio_pipeline_terminate(mPipeline));
    if (mInputType != kInputNone) {
        destroyInputSide();
    }
    destroyOutputSide();
    ESP_ERROR_CHECK(audio_pipeline_deinit(mPipeline));
    mPipeline = nullptr;
}

int AudioPlayer::httpEventHandler(http_stream_event_msg_t *msg)
{
//    ESP_LOGI("STREAM", "http stream event %d, heap free: %d", msg->event_id, xPortGetFreeHeapSize());
    if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS) {
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        return http_stream_fetch_again(msg->el);
    }
    return ESP_OK;
}
const char* AudioPlayer::codecTypeToStr(CodecType type)
{
    switch (type) {
        case kCodecMp3: return "mp3";
        case kCodecAac: return "aac";
        case kCodecNone: return "none";
        default: return "(unknown)";
    }
}
