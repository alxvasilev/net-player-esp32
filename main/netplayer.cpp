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
#include <esp_spiffs.h>
#include <sys/param.h>
#include <string>
#include <memory>
#include "utils.hpp"
#include "netLogger.hpp"
#include "httpFile.hpp"

typedef enum { kOutputTypeI2s = 1, kOutputTypeA2dp } OutputType;
static constexpr gpio_num_t kPinButton = GPIO_NUM_27;

static const char *TAG = "netplay";
static const char* kStreamUrls[] = {
    "http://stream01048.westreamradio.com:80/wsm-am-mp3",
    "http://94.23.252.14:8067/player"
};

        // "http://icestreaming.rai.it/12.mp3");
        // "http://94.23.252.14:8067/player");
        // "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3");
        // BBC m4a "http://a.files.bbci.co.uk/media/live/manifesto/audio/simulcast/hls/nonuk/sbr_low/llnw/bbc_radio_one.m3u8"

httpd_handle_t gHttpServer = nullptr;
void reconfigDhcpServer();
void startWifiSoftAp();
void startWebserver(bool isAp=false);

audio_element_handle_t
    http_stream_reader, decompressor, equalizer, streamOut;
OutputType outputType;
audio_pipeline_handle_t pipeline;
esp_periph_set_handle_t periphSet;

int httpEventHandler(http_stream_event_msg_t *msg);
const char* getNextStreamUrl() {
    static int currStreamIdx = -1;
    currStreamIdx++;
    if (currStreamIdx >= (sizeof(kStreamUrls) / sizeof(kStreamUrls[0]))) {
        currStreamIdx = 0;
    }
    return kStreamUrls[currStreamIdx];
}

audio_element_handle_t createOutputI2s()
{
    ESP_LOGI(TAG, "Creating i2s output to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = myI2S_STREAM_INTERNAL_DAC_CFG_DEFAULT;
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    audio_element_handle_t elem = i2s_stream_init(&i2s_cfg);
    outputType = kOutputTypeI2s;
    return elem;
}
audio_element_handle_t createOutputA2dp()
{
    ESP_LOGI(TAG, "Creating a2dp output source");
    ESP_LOGI(TAG, "\tCreating Bluetooth service");
    bluetooth_service_cfg_t cfg;
    cfg.device_name = "ESP-ADF-SOURCE";
    cfg.mode = BLUETOOTH_A2DP_SOURCE;
    cfg.remote_name = "DL-LINK";
    bluetooth_service_start(&cfg);

    ESP_LOGI(TAG, "\tCreating bluetooth sink element");
    audio_element_handle_t elem = bluetooth_service_create_stream();
    outputType = kOutputTypeA2dp;

    const uint8_t* addr = esp_bt_dev_get_address();
    char strAddr[13];
    binToHex(addr, 6, strAddr);
    ESP_LOGW("BT", "Own BT MAC: '%s'", strAddr);
// Move this to execute only once
    ESP_LOGI(TAG, "\tCreating and starting Bluetooth peripheral");
    esp_periph_handle_t btPeriph = bluetooth_service_create_periph();
    esp_periph_start(periphSet, btPeriph);
    return elem;
}
void configGpios()
{
    gpio_pad_select_gpio(kPinButton);
    gpio_set_direction(kPinButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinButton);
}

NetLogger netLogger(false);

void rollbackConfirmAppIsWorking()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t otaState;
    if (esp_ota_get_state_partition(running, &otaState) != ESP_OK) {
        return;
    }
    if (otaState != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGW(TAG, "App appears to be working properly, confirming boot partition...");
    esp_ota_mark_app_valid_cancel_rollback();
};
void testSpiffs()
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

     esp_vfs_spiffs_conf_t conf = {
       .base_path = "/spiffs",
       .partition_label = "storage",
       .max_files = 5,
       .format_if_mount_failed = true
     };

     // Use settings defined above to initialize and mount SPIFFS filesystem.
     // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
     esp_err_t ret = esp_vfs_spiffs_register(&conf);

     if (ret != ESP_OK) {
         if (ret == ESP_FAIL) {
             ESP_LOGE(TAG, "Failed to mount or format filesystem");
         } else if (ret == ESP_ERR_NOT_FOUND) {
             ESP_LOGE(TAG, "Failed to find SPIFFS partition");
         } else {
             ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
         }
         return;
     }

     size_t total = 0, used = 0;
     ret = esp_spiffs_info(conf.partition_label, &total, &used);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
     } else {
         ESP_LOGW(TAG, "Partition size: total: %d, used: %d", total, used);
     }
}
extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    configGpios();

    testSpiffs();
    tcpip_adapter_init();
    rollbackConfirmAppIsWorking();
    if (!gpio_get_level(kPinButton)) {
        ESP_LOGW(TAG, "Button pressed at boot, start as access point for configuration");
        startWifiSoftAp();
        startWebserver(true);
        return;
    }
//== WIFI
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periphSet = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "Start and wait for Wi-Fi network");
    periph_wifi_cfg_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    wifi_cfg.ssid = CONFIG_WIFI_SSID;
    wifi_cfg.password = CONFIG_WIFI_PASSWORD;
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(periphSet, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
//====
    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = myHTTP_STREAM_CFG_DEFAULT;
//  http_cfg.multi_out_num = 1;
    http_cfg.enable_playlist_parser = 1;
    http_cfg.event_handle = httpEventHandler;
    http_stream_reader = http_stream_init(&http_cfg);

    streamOut = createOutputI2s();
//  streamOut = createOutputA2dp();

    ESP_LOGI(TAG, "[2.3] Create decompressor to decode mp3/aac/etc");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    decompressor = mp3_decoder_init(&mp3_cfg);

//    aac_decoder_cfg_t aac_cfg = DEFAULT_AAC_DECODER_CONFIG();
//    decompressor = aac_decoder_init(&aac_cfg);

    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    int gainTable[] = { 10, 10, 8, 4, 2, 0, 0, 2, 4, 6,
                       10, 10, 8, 4, 2, 0, 0, 2, 4, 6};
    // The size of gain array should be the multiplication of NUMBER_BAND
    // and number channels of audio stream data. The minimum of gain is -13 dB.
    eq_cfg.set_gain = gainTable;
    equalizer = equalizer_init(&eq_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, decompressor, "decomp");
    audio_pipeline_register(pipeline, equalizer, "eq");
    audio_pipeline_register(pipeline, streamOut, "out");

    ESP_LOGI(TAG, "[2.5] Link elements together http_stream-->mp3_decoder-->equalizer-->i2s_stream-->[codec_chip]");
    const char* order[] = {"http", "decomp", "eq", "out"};
    audio_pipeline_link(pipeline, order, 4);
    const char* url = getNextStreamUrl();
    ESP_LOGI(TAG, "[2.6] Set http stream uri to '%s'", url);
    audio_element_set_uri(http_stream_reader, url);

    ESP_LOGI(TAG, "[ 5 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[5.1] Listening event from all elements of audio pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[5.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(periphSet), evt);

    ESP_LOGI(TAG, "[ 6 ] Start pipeline");
    audio_pipeline_run(pipeline);
    startWebserver();
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
            && msg.source == (void *) decompressor
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
    audio_pipeline_unregister_more(pipeline, http_stream_reader,
        decompressor, equalizer, streamOut, NULL);
    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    esp_periph_set_stop_all(periphSet);
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(periphSet), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(streamOut);
    audio_element_deinit(decompressor);
    audio_element_deinit(equalizer);
    esp_periph_set_destroy(periphSet);
}

void changeStreamUrl(const char* url)
{
    ESP_LOGW("STREAM", "Changing stream URL to '%s'", url);
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_element_set_uri(http_stream_reader, url);
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

esp_err_t OTA_update_post_handler(httpd_req_t *req);
static const httpd_uri_t ota = {
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = OTA_update_post_handler,
    .user_ctx  = NULL
};

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
    httpd_register_uri_handler(gHttpServer, &ota);
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

