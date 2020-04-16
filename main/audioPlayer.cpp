#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include "audio_common.h"
#include "http_stream.h"
#include "equalizer.h"
#include "i2s_stream.h"
#include <mp3_decoder.h>
#include <aac_decoder.h>
#include <ogg_decoder.h>
#include <flac_decoder.h>
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


const uint16_t AudioPlayer::equalizerFreqs[10] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

#define LOCK_PLAYER() MutexLocker locker(mutex)

void AudioPlayer::createInputHttp()
{
    assert(!mStreamIn);
    ESP_LOGI("HTTP", "Create http stream reader");
    http_stream_cfg_t cfg = myHTTP_STREAM_CFG_DEFAULT;
    cfg.enable_playlist_parser = 1;
    cfg.auto_connect_next_track = 1;
    cfg.event_handle = httpStreamEventHandler;
    cfg.user_data = this;
    cfg.out_rb_size = kHttpBufSize;
    mInputType = kInputHttp;
    mStreamIn = http_stream_init(&cfg);
    assert(mStreamIn);
    audio_element_set_event_callback(mStreamIn, httpElementEventHandler, this);
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
    audio_element_set_event_callback(mStreamIn, inputFormatEventCb, this);

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
    cfg.use_alc = 0;
    mStreamOut = i2s_stream_init(&cfg);
    assert(mStreamOut);
    audio_element_set_event_callback(mStreamOut, outputEventCb, this);
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

void AudioPlayer::createDecoderByType(esp_codec_type_t type)
{
    assert(!mDecoder);
    mDecoderType = type;
    switch (type) {
    case ESP_CODEC_TYPE_MP3: {
        mp3_decoder_cfg_t cfg = DEFAULT_MP3_DECODER_CONFIG();
        cfg.task_core = 1;
        mDecoder = mp3_decoder_init(&cfg);
        break;
    }
    case ESP_CODEC_TYPE_AAC: {
        aac_decoder_cfg_t cfg = DEFAULT_AAC_DECODER_CONFIG();
        mDecoder = aac_decoder_init(&cfg);
        break;
    }
    case ESP_CODEC_TYPE_OGG: {
        ogg_decoder_cfg_t cfg = DEFAULT_OGG_DECODER_CONFIG();
        mDecoder = ogg_decoder_init(&cfg);
        break;
    }
    case ESP_CODEC_TYPE_FLAC: {
        flac_decoder_cfg_t cfg = DEFAULT_FLAC_DECODER_CONFIG();
        mDecoder = flac_decoder_init(&cfg);
        break;
    }
    default:
        mDecoder = nullptr;
        mDecoderType = ESP_CODEC_TYPE_UNKNOW;
        assert(false);
        return;
    }
    assert(mDecoderType == type);
    audio_element_set_event_callback(mDecoder, inputFormatEventCb, this);
}

void AudioPlayer::changeDecoder(esp_codec_type_t type)
{
    ESP_LOGW(TAG, "Changing decoder %s --> %s",
        codecTypeToStr(mDecoderType), codecTypeToStr(type));
    auto inType = mInputType;
    auto outType = mOutputType;
    const char* uri = audio_element_get_uri(mStreamIn);
    std::string url;
    if (uri) {
        url = uri;
    }
    destroyPipeline();
    createPipeline(inType, type, outType);
    if (!url.empty()) {
        audio_element_set_uri(mStreamIn, url.c_str());
    }
}

esp_err_t AudioPlayer::inputFormatEventCb(audio_element_handle_t el,
    audio_event_iface_msg_t* msg, void* ctx)
{
    assert(ctx);
    auto self = static_cast<AudioPlayer*>(ctx);
    if (msg->cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t info;
        memset(&info, 0, sizeof(info));
        audio_element_getinfo(el, &info);

        ESP_LOGI(TAG, "Received music info from samplerate source:\n"
            "samplerate: %d, bits: %d, ch: %d, bps: %d, codec: %d",
            info.sample_rates, info.bits, info.channels, info.bps, info.codec_fmt);
        if (self->mEqualizer) {
            equalizer_set_info(self->mEqualizer, info.sample_rates, info.channels);
        }
        audio_element_setinfo(self->mStreamOut, &info);
        if (self->mOutputType == kOutputI2s) {
            i2s_stream_set_clk(self->mStreamOut, info.sample_rates, info.bits, info.channels);
        }
    }
    return ESP_OK;
}

esp_err_t AudioPlayer::outputEventCb(audio_element_handle_t el,
    audio_event_iface_msg_t* msg, void* ctx)
{
    if (msg->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        int status = (int)msg->data;
        if (status == AEL_STATUS_STATE_STOPPED || status == AEL_STATUS_STATE_FINISHED) {
            auto self = static_cast<AudioPlayer*>(ctx);
            self->mState = kStateStopped;
            ESP_LOGI(TAG, "Stopped (output state transitioned to stopped)");
        }
        ESP_LOGI("OUT", "Unhandled status event %d", status);
    } else {
        ESP_LOGI("OUT", "Unhandled event %d", msg->cmd);
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

AudioPlayer::AudioPlayer(InputType inType, esp_codec_type_t codecType,
                         OutputType outType, bool useEq)
:mFlags(useEq ? kFlagUseEqualizer : (Flags)0)
{
    createPipeline(inType, codecType, outType);
}

void AudioPlayer::createPipeline(InputType inType, esp_codec_type_t codecType, OutputType outType)
{
    ESP_LOGI(TAG, "Create audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    mPipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(mPipeline);

    if (inType == kInputHttp) {
        createInputHttp();
        createDecoderByType(codecType);
    } else if (inType == kInputA2dp) {
        createInputA2dp();
        mDecoder = nullptr;
    }

    if (mFlags & kFlagUseEqualizer) {
        createEqualizer();
    } else {
        mEqualizer = nullptr;
    }
    createOutputElement(outType);
    registerAllAndLinkPipeline();
}

void AudioPlayer::destroyPipeline()
{
    if (!mPipeline) {
        return;
    }
    stop();
    audio_pipeline_deinit(mPipeline);
    mPipeline = nullptr;
    mStreamIn = mDecoder = mEqualizer = mStreamOut = nullptr;
    mInputType = kInputNone;
    mOutputType = kOutputNone;
    mDecoderType = ESP_CODEC_TYPE_UNKNOW;
}

void AudioPlayer::playUrl(const char* url, esp_codec_type_t codecType)
{
    LOCK_PLAYER();
    assert(mStreamIn && mInputType == kInputHttp);

    if (codecType != ESP_CODEC_TYPE_UNKNOW && (mDecoderType != codecType)) {
        changeDecoder(codecType);
    }

    ESP_LOGI(TAG, "Setting http stream uri to '%s', format: %s", url, codecTypeToStr(codecType));
    ESP_LOGI(TAG, "setSourceUrl: current state is %d", mState);
    if ((mState != kStateStopped)) {
        stop();
    }
    ESP_ERROR_CHECK(audio_element_set_uri(mStreamIn, url));
    play();
}

void AudioPlayer::registerAllAndLinkPipeline()
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
}

void AudioPlayer::play()
{
    LOCK_PLAYER();
    if (mState == kStatePlaying) {
        ESP_LOGW(TAG, "AudioPlayer::play: already playing");
    } else if (mState == kStatePaused) {
        audio_element_resume(mStreamIn, 0, portMAX_DELAY);
    } else {
        // Listening event from all elements of audio pipeline
        // NOTE: This must be re-applied after pipeline change
        ESP_LOGI(TAG, "Starting pipeline");
        audio_pipeline_run(mPipeline);
        if ((mFlags & kFlagNoWaitPrefill) == 0) {
            audio_element_pause(mStreamOut);
        }
    }
    mState = kStatePlaying;
}

void AudioPlayer::pause()
{
    LOCK_PLAYER();
    assert(mState = kStatePlaying);
    ESP_ERROR_CHECK(audio_pipeline_pause(mPipeline));
    mState = kStatePaused;
}

void AudioPlayer::resume()
{
    LOCK_PLAYER();
    assert(mState == kStatePaused);
    ESP_ERROR_CHECK(audio_pipeline_resume(mPipeline));
    mState = kStatePlaying;
}

void AudioPlayer::stop()
{
//   LOCK_PLAYER();
    if (mState == kStateStopped) {
        ESP_LOGW(TAG, "stop: already stopped");
        return;
    }
    ESP_LOGI(TAG, "Stopping pipeline");
    if (audio_pipeline_stop(mPipeline) == ESP_OK) {
        ESP_ERROR_CHECK(audio_pipeline_wait_for_stop(mPipeline));
    }
    mState = kStateStopped;
    ESP_LOGI(TAG, "Pipeline stopped");
}

void AudioPlayer::destroyDecoder()
{
    LOCK_PLAYER();
    if (!mDecoder) {
        return;
    }
    ESP_ERROR_CHECK(audio_pipeline_breakup_elements(mPipeline, nullptr));
    ESP_ERROR_CHECK(audio_pipeline_unregister(mPipeline, mDecoder));
    ESP_ERROR_CHECK(audio_element_deinit(mDecoder));
    mDecoder = nullptr;
    mDecoderType = ESP_CODEC_TYPE_UNKNOW;
}

bool AudioPlayer::volumeSet(int vol)
{
    LOCK_PLAYER();
    if (mOutputType == kOutputI2s) {
        ESP_ERROR_CHECK(i2s_alc_volume_set(mStreamOut, vol));
        return true;
    }
    return false;
}
int AudioPlayer::volumeGet()
{
    LOCK_PLAYER();
    if (mOutputType == kOutputI2s) {
        int vol;
        ESP_ERROR_CHECK(i2s_alc_volume_get(mStreamOut, &vol));
        return vol;
    } else {
        return -1;
    }
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
    } else if (newVol > 100) {
        newVol = 100;
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
    LOCK_PLAYER();
    if (!mEqualizer) {
        return false;
    }
    return equalizer_set_gain_info(mEqualizer, band, level, 1) == ESP_OK;
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
        if (gain < 50 || gain > 50) {
            ok = false;
        }
        ok &= (equalizer_set_gain_info(mEqualizer, band, gain, 1) != ESP_OK);
    }
    return ok;
}

int* AudioPlayer::equalizerDumpGains()
{
    equalizer_cfg_t cfg = DEFAULT_EQUALIZER_CONFIG();
    return cfg.set_gain;
}

AudioPlayer::~AudioPlayer()
{
    destroyPipeline();
}

int AudioPlayer::httpStreamEventHandler(http_stream_event_msg_t *msg)
{
    //ESP_LOGI("STREAM", "http stream event %d, heap free: %d", msg->event_id, xPortGetFreeHeapSize());
    // NOTE: We don't lock the player here, because this will cause a deadlock in
    // stop() waiting for http task to terminate, and http task waiting to acquire
    // the player mutex here. To provide safety, the parts of the player accessed
    // here (mStreamOut) must not be changed while the http task is running.
    if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
        auto self = static_cast<AudioPlayer*>(msg->user_data);
        if (self->mState != kStatePlaying) {
            return ESP_OK;
        }
        auto rb = audio_element_get_output_ringbuf(msg->el);
        auto bytesInBuf = rb_bytes_filled(rb);
        ESP_LOGI("http", "data: ringbuf: %d, heap free: %d", bytesInBuf, xPortGetFreeHeapSize());
        if (bytesInBuf >= kHttpBufSize - 1024) {
            if (audio_element_get_state(self->mStreamOut) == AEL_STATE_PAUSED) {
                audio_element_resume(self->mStreamOut, 0, 0);
                ESP_LOGW("HTTP", "Input buffer filled, output resumed");
            }
        } else if (bytesInBuf < 1024) {
            if (audio_element_get_state(self->mStreamOut) == AEL_STATE_RUNNING) {
                audio_element_pause(self->mStreamOut);
                ESP_LOGW("HTTP", "About to underflow, stopped playback until buffer fills");
            }
        }
        return ESP_OK;
    }
    else if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS) {
        return ESP_OK;
    }
    else if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    else if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        // if we got gracefully disconnected, http client assumes we have a playlist
        // and it's the end of the track and we tell it to go to the next one.
        // However, there may be no playlist and we need to reconnect. In that case
        // returning ESP_OK will trigger that reconnect
        http_stream_fetch_again(msg->el);
        return ESP_OK;
    } else {
        return ESP_OK;
    }
}
esp_err_t AudioPlayer::httpElementEventHandler(audio_element_handle_t el,
    audio_event_iface_msg_t* msg, void* ctx)
{
    if (msg->cmd == AEL_MSG_CMD_REPORT_CODEC_FMT) {
        auto self = static_cast<AudioPlayer*>(ctx);
        audio_element_info_t info;
        audio_element_getinfo(el, &info);
        auto codec = info.codec_fmt;
        ESP_LOGW(TAG, "Stream codec is %s", codecTypeToStr(codec));
        if (codec != self->mDecoderType) {
            audio_element_stop(el);
            setTimeout(0, [self, codec] {
                MutexLocker locker(self->mutex);
                self->changeDecoder(codec);
                ESP_LOGW(TAG, "Decoder changed, playing");
                self->play();
            });
        }
    }
    return ESP_OK;
}

const char* AudioPlayer::codecTypeToStr(esp_codec_type_t type)
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
