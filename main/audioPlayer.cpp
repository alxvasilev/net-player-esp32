#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "equalizer.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "aac_decoder.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "bluetooth_service.h"
#include <esp_ota_ops.h>
#include "board.h"
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_bt_device.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <a2dp_stream.h>
#include <esp_spiffs.h>
#include <sys/param.h>
#include <string>
#include <memory>
#include "utils.hpp"
#include "netLogger.hpp"
#include "httpFile.hpp"
#include "ota.hpp"

static constexpr const char* TAG = "PIPELINE";

class AudioPlayer
{
public:
    enum InputType
    { kInputNone = 0, kInputHttp = 1, kInputA2dp };
    enum OutputType
    { kOutputNone = 0, kOutputI2s, kOutputA2dp };
    enum CodecType
    { kCodecNone = 0, kCodecMp3, kCodecAac };
protected:
    esp_periph_set_handle_t mPeriphSet;
    InputType mInputType = kInputNone;
    OutputType mOutputType = kOutputNone;
    CodecType mDecoderType = kCodecNone;
    bool mUseEqualizer = true;
    audio_element_handle_t mStreamIn = nullptr;
    audio_element_handle_t mDecoder = nullptr;
    audio_element_handle_t mEqualizer = nullptr;
    audio_element_handle_t mStreamOut = nullptr;
    audio_pipeline_handle_t mPipeline = nullptr;
    audio_event_iface_handle_t mEventListener = nullptr;
    static constexpr int mEqualizerDefaultGainTable[] = {
        10, 10, 8, 4, 2, 0, 0, 2, 4, 6,
        10, 10, 8, 4, 2, 0, 0, 2, 4, 6
    };

    static int httpEventHandler(http_stream_event_msg_t *msg);
    void createInputHttp();
    void createInputA2dp();

    void createOutputI2s();
    void createOutputA2dp();
    void createOutputElement();

    void createDecoderByType(CodecType type);
    void createAndSetupEqualizer();
    void createOutputSide(OutputType outType);
    void createInputSide(InputType inType, CodecType codecType);
    void createEventListener();
public:
    AudioPlayer(OutputType outType, bool useEq=true);

};

void AudioPlayer::createInputHttp()
{
    assert(!mStreamIn);
    ESP_LOGI("HTTP", "Create http stream reader");
    http_stream_cfg_t cfg = myHTTP_STREAM_CFG_DEFAULT;
    //  http_cfg.multi_out_num = 1;
    cfg.enable_playlist_parser = 1;
    cfg.event_handle = httpEventHandler;
    mInputType = kInputHttp;
    mStreamIn = http_stream_init(&cfg);
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
    mInputType = kInputA2dp;

    ESP_LOGI(BT, "Create and start Bluetooth peripheral");
    auto bt_periph = bt_create_periph();
    esp_periph_start(mPeriphSet, bt_periph);
}

void AudioPlayer::createOutputI2s()
{
    assert(!mStreamOut);
    ESP_LOGI(TAG, "Creating i2s output to write data to codec chip");
    i2s_stream_cfg_t cfg = myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
    cfg.type = AUDIO_STREAM_WRITER;
    mStreamOut = i2s_stream_init(&cfg);
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
    bluetooth_service_start(&cfg);

    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    mStreamOut = bluetooth_service_create_stream();

    const uint8_t* addr = esp_bt_dev_get_address();
    char strAddr[13];
    binToHex(addr, 6, strAddr);
    ESP_LOGW("BT", "Own BT MAC: '%s'", strAddr);
//  Move this to execute only once
    ESP_LOGI(TAG, "\tCreating and starting Bluetooth peripheral");
    esp_periph_handle_t btPeriph = bluetooth_service_create_periph();
    esp_periph_start(mPeriphSet, btPeriph);
}

void AudioPlayer::createOutputElement()
{
    assert(!mStreamOut);
    switch(mOutputType) {
    case kOutputI2s: {
        createOutputI2s();
        return;
    }
    case kOutputA2dp: {
        createOutputA2dp();
        return;
    }
    default:
        assert(false);
    }
}

void AudioPlayer::createDecoderByType(CodecType type)
{
    assert(!mDecoder);
    mDecoderType = type;
    switch (type) {
    case kCodecMp3: {
        mp3_decoder_cfg_t cfg = DEFAULT_MP3_DECODER_CONFIG();
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
        break;
    }
}

void AudioPlayer::createAndSetupEqualizer()
{
    equalizer_cfg_t cfg = DEFAULT_EQUALIZER_CONFIG();
    // The size of gain array should be the multiplication of NUMBER_BAND
    // and number channels of audio stream data. The minimum of gain is -13 dB.
    // TODO: Load equalizer from nvs
    memcpy(cfg.set_gain, mEqualizerDefaultGainTable, sizeof(mEqualizerDefaultGainTable));
    mEqualizer = equalizer_init(&cfg);
}

AudioPlayer::AudioPlayer(OutputType outType, bool useEq)
:mUseEqualizer(useEq)
{
    createEventListener();
    createOutputSide(outType);
}

void AudioPlayer::createOutputSide(OutputType outType)
{
    mOutputType = outType;
    ESP_LOGI(TAG, "Create audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    mPipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(mPipeline);
    if (mUseEqualizer) {
        createAndSetupEqualizer();
    } else {
        mEqualizer = nullptr;
    }
    createOutputElement();
}
// const char* url = getNextStreamUrl();
bool AudioPlayer::setSourceUrl(const char* url)
{
    if (mInputType != kInputHttp) {
        return false;
    }
    ESP_LOGI(TAG, "Set http stream uri to '%s'", url);
    audio_element_set_uri(mStreamIn, url);
}

void AudioPlayer::createInputSide(InputType inType, CodecType codecType)
{
    if (inType == kInputHttp) {
        createInputHttp();
        createDecoderByType(codecType);
    } else if (inType == kInputA2dp) {
        createInputA2dp();
        mDecoder = nullptr;
    }
}

void AudioPlayer::linkPipeline()
{
    ESP_LOGI(TAG, "Registering and linking pipeline elements");
    std::vector<const char*> order;
    order.reserve(4);
    order.push_back("in");
    audio_pipeline_register(mPipeline, mStreamIn, order.back());
    if (mDecoder) {
        order.push_back("dec");
        audio_pipeline_register(mPipeline, mDecoder, order.back());
    }
    if (mEqualizer) {
        order.push_back("eq");
        audio_pipeline_register(mPipeline, mEqualizer, order.back());
    }
    order.push_back("out");
    audio_pipeline_register(mPipeline, mStreamOut, order.back());

    audio_pipeline_link(pipeline, order.data(), order.size());
}

void AudioPlayer::createEventListerner()
{
    ESP_LOGI(TAG, "Set up event listener");
    audio_event_iface_cfg_t cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    mEventListener = audio_event_iface_init(&cfg);
    // Listen for events from peripherals
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(mPeriphSet), mEventListener);
}

void AudioPlayer::play()
{
    // Listening event from all elements of audio pipeline
    // NOTE: This must be re-applied after pipeline change
    audio_pipeline_set_listener(mPipeline, mEventListener);

    ESP_LOGI(TAG, "Starting pipeline");
    audio_pipeline_run(mPipeline);
    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }
//        ESP_LOGI("source_type = %d, source: %p, cmd: %d\n",
//            msg.source_type, msg.source, msg.cmd);

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) decompressor && decompressor
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info;
            memset(&music_info, 0, sizeof(music_info));
            audio_element_getinfo(decompressor, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(streamOut, &music_info);
            if (outputType == kOutputTypeI2s) {
                i2s_stream_set_clk(streamOut, music_info.sample_rates, music_info.bits, music_info.channels);
            }
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
                && msg.source == (void *) streamIn && inputType == kInputTypeA2dp
                && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info;
            memset(&music_info, 0, sizeof(music_info));
            audio_element_getinfo(streamIn, &music_info);
            ESP_LOGI(TAG, "[ * ] Receive music info from a2dp input, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(streamOut, &music_info);
            if (outputType == kOutputTypeI2s) {
                i2s_stream_set_clk(streamOut, music_info.sample_rates, music_info.bits, music_info.channels);
            }
            continue;
        }
        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.source == (void*) streamOut &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
        } else if (msg.source_type == PERIPH_ID_BLUETOOTH && msg.source == (void*)streamOut) {
            ESP_LOGW("myBT", "Bluetooth event cmd = %d", msg.cmd);
        }
    }

    ESP_LOGI(TAG, "[ 7 ] Stop pipelines");
    audio_pipeline_terminate(pipeline);
    audio_pipeline_unregister(pipeline, streamIn);
    if (decompressor) {
        audio_pipeline_unregister(pipeline, decompressor);
    }
    if (equalizer) {
        audio_pipeline_unregister(pipeline, equalizer);
    }
    audio_pipeline_unregister(pipeline, streamOut);
    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    esp_periph_set_stop_all(periphSet);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(periphSet), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(streamIn);
    audio_element_deinit(streamOut);
    if (decompressor) {
        audio_element_deinit(decompressor);
    }
    if (equalizer) {
        audio_element_deinit(equalizer);
    }
    esp_periph_set_destroy(periphSet);
}

void changeStreamUrl(const char* url)
{
    ESP_LOGW("STREAM", "Changing stream URL to '%s'", url);
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_element_set_uri(streamIn, url);
    audio_pipeline_resume(pipeline);
}

static esp_err_t indexUrlHandler(httpd_req_t *req)
{
    static const char indexHtml[] =
            "<html><head /><body><h>NetPlayer HTTP server</h><br/>Free heap memory: ";
    httpd_resp_send_chunk(req, indexHtml, sizeof(indexHtml));
    char buf[32];
    snprintf(buf, sizeof(buf)-1, "%d", xPortGetFreeHeapSize());
    httpd_resp_send_chunk(req, buf, strlen(buf));
    static const char indexHtmlEnd[] = "</body></html>";
    httpd_resp_send_chunk(req, indexHtmlEnd, sizeof(indexHtmlEnd));
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}
static const httpd_uri_t indexUrl = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = indexUrlHandler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = nullptr
};

/* An HTTP GET handler */
static esp_err_t playUrlHandler(httpd_req_t *req)
{
    UrlParams params(req);
    for (auto& param: params.keyVals()) {
        ESP_LOGI("URL", "'%s' = '%s'", param.key.str, param.val.str);
    }
    auto url = params.strParam("url");
    if (!url) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL parameter not found");
        ESP_LOGE("http", "Url param not found, query:'%s'", params.ptr());
        return ESP_OK;
    }
    ESP_LOGW("HTTP", "Http req url: %s", url.str);
    auto strUrl = getNextStreamUrl();
    changeStreamUrl(strUrl);
    std::string msg("Changing stream url to '");
    msg.append(strUrl).append("'");
    httpd_resp_send(req, msg.c_str(), msg.size());
    return ESP_OK;
}

static const httpd_uri_t play = {
    .uri       = "/play",
    .method    = HTTP_GET,
    .handler   = playUrlHandler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = nullptr
};

int httpEventHandler(http_stream_event_msg_t *msg)
{
    ESP_LOGI("STREAM", "http stream event %d, heap free: %d", msg->event_id, xPortGetFreeHeapSize());
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

void stopWebserver() {
    /* Stop the web server */
    if (!gHttpServer) {
        return;
    }
    netLogger.unregisterWithHttpServer("/log");
    httpd_stop(gHttpServer);
    gHttpServer = nullptr;
}

void startWebserver(bool isAp)
{
    if (gHttpServer) {
        stopWebserver();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&gHttpServer, &config) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting server!");
        return;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    netLogger.registerWithHttpServer(gHttpServer, "/log");
    httpd_register_uri_handler(gHttpServer, &otaUrlHandler);
    httpd_register_uri_handler(gHttpServer, &indexUrl);
    httpd_register_uri_handler(gHttpServer, &httpFsPut);
    httpd_register_uri_handler(gHttpServer, &httpFsGet);

    if (!isAp) {
        httpd_register_uri_handler(gHttpServer, &play);
    }
}

void reconfigDhcpServer()
{
    // stop DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    // assign a static IP to the network interface
    tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));

    IP4_ADDR(&info.ip, 192, 168, 0, 1);
    IP4_ADDR(&info.gw, 192, 168, 0, 1); //ESP acts as router, so gw addr will be its own addr
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));
    // start the DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
    printf("DHCP server started \n");
}

static esp_err_t apEventHandler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    default:
        break;
    }
    return ESP_OK;
}
void startWifiSoftAp()
{
    ESP_ERROR_CHECK(esp_event_loop_init(apEventHandler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    //ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

//  reconfigDhcpServer();

    // configure the wifi connection and start the interface
    wifi_config_t ap_config;
    auto& ap = ap_config.ap;
    strcpy((char*)ap.ssid, "NetPlayer");
    strcpy((char*)ap.password, "net12player");
    ap.ssid_len = 0;
    ap.channel = 4;
    ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ssid_hidden = 0;
    ap.max_connection = 8;
    ap.beacon_interval = 400;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("ESP WiFi started in AP mode \n");
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(20));
}

