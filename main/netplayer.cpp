#include <string.h>
#include "freertos/FreeRTOS.h"
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <sys/param.h>
#include <string>
#include <algorithm> // for sorting task list
#include <memory>
#include "utils.hpp"
#include "netLogger.hpp"
#include "httpFile.hpp"
#include "ota.hpp"
#include "audioPlayer.hpp"
#include "wifi.hpp"
#include "bluetooth.hpp"
#include "taskList.hpp"
#include <st7735.hpp>
#include "sdcard.hpp"

static constexpr gpio_num_t kPinButton = GPIO_NUM_27;
static constexpr gpio_num_t kPinRollbackButton = GPIO_NUM_32;
static constexpr gpio_num_t kPinLed = GPIO_NUM_2;

static const char *TAG = "netplay";
static const char gPlaylist[] =
"http://streams.greenhost.nl:8080/live\n\
http://78.129.150.144/stream.mp3?ipport=78.129.150.144_5064\n\
http://stream01048.westreamradio.com:80/wsm-am-mp3\n\
https://mediaserv38.live-streams.nl:18030/stream\n\
http://94.23.252.14:8067/player\n\
http://italo.italo.nu/live";


        // "http://icestreaming.rai.it/12.mp3");
        // "http://94.23.252.14:8067/player");
        // "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3");
        // BBC m4a "http://a.files.bbci.co.uk/media/live/manifesto/audio/simulcast/hls/nonuk/sbr_low/llnw/bbc_radio_one.m3u8"

httpd_handle_t gHttpServer = nullptr;
std::unique_ptr<AudioPlayer> player;
std::unique_ptr<WifiBase> wifi;
TaskList taskList;
ST7735Display lcd(VSPI_HOST);
SDCard sdcard;

void startWebserver(bool isAp=false);

void configGpios()
{
    gpio_pad_select_gpio(kPinButton);
    gpio_set_direction(kPinButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinButton);

    gpio_pad_select_gpio(kPinRollbackButton);
    gpio_set_direction(kPinRollbackButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinRollbackButton);

    gpio_pad_select_gpio(kPinLed);
    gpio_set_direction(kPinLed, GPIO_MODE_OUTPUT);
}

NetLogger netLogger(false);
void blinkLed(int dur, int periodMs, uint8_t duty10=5)
{
    int ticksOn = (periodMs * duty10 / 10) / portTICK_PERIOD_MS;
    int ticksOff = (periodMs * (10 - duty10) / 10) / portTICK_PERIOD_MS;
    for(int i = dur / periodMs; i > 0; i--) {
        gpio_set_level(kPinLed, 1);
        vTaskDelay(ticksOn);
        gpio_set_level(kPinLed, 0);
        vTaskDelay(ticksOff);
    }
}

void blinkLedProgress(int dur, int periodMs)
{
    int count = dur / periodMs;
    for(int i = 0; i < count; i++) {
        gpio_set_level(kPinLed, 1);
        vTaskDelay((periodMs * i / count) / portTICK_PERIOD_MS);
        gpio_set_level(kPinLed, 0);
        vTaskDelay((periodMs * (count - i) / count) / portTICK_PERIOD_MS);
    }
}

void initNvs()
{
    /* Initialize NVS — it is used to store PHY calibration data */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void mountSpiffs()
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
bool rollbackCheckUserForced()
{
    if (gpio_get_level(kPinRollbackButton)) {
        return false;
    }

    static constexpr const char* RB = "ROLLBACK";
    ESP_LOGW("RB", "Rollback button press detected, waiting for 4 second to confirm...");
    blinkLedProgress(4000, 200);
    if (gpio_get_level(kPinRollbackButton)) {
        ESP_LOGW("RB", "Rollback not pressed after 1 second, rollback canceled");
        return false;
    }
    ESP_LOGW(RB, "App rollback requested by user button, rolling back and rebooting...");
    setOtherPartitionBootableAndRestart();
    return true;
}
static constexpr ST7735Display::PinCfg lcdPins = {
    {
        .clk = GPIO_NUM_18,
        .mosi = GPIO_NUM_23,
        .cs = GPIO_NUM_5,
    },
    .dc = GPIO_NUM_33, // data/command
    .rst = GPIO_NUM_4
};

extern "C" void app_main(void)
{
//  esp_log_level_set("*", ESP_LOG_DEBUG);
    configGpios();

    rollbackCheckUserForced();
    rollbackConfirmAppIsWorking();

    lcd.init(300, 240, lcdPins);
    lcd.setFont(Font_7x11);
    lcd.puts("Mounting storage...\n");
    initNvs();
    mountSpiffs();

    if (!gpio_get_level(kPinButton)) {
        ESP_LOGW(TAG, "Button pressed at boot, start as access point for configuration");
        wifi.reset(new WifiAp);
        static_cast<WifiAp*>(wifi.get())->start("netplayer", "net12player", 8);
        startWebserver(true);
        return;
    }

    lcd.puts("Connecting to WiFi...\n");

    //== WIFI
    wifi.reset(new WifiClient);
    static_cast<WifiClient*>(wifi.get())->start(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    if (!wifi->waitForConnect(20000)) {
        lcd.puts("Timed out, starting AP\n");
        lcd.puts("ssid: netplayer\n");
        lcd.puts("key: alexisthebest\n");
        wifi.reset(new WifiAp);
        static_cast<WifiAp*>(wifi.get())->start("netplayer", "alexisthebest", 1);
    }
 //====
    lcd.puts("Starting webserver...\n");
    startWebserver();    
/*
    lcd.puts("Waiting log conn...\n");
    netLogger.waitForLogConnection();
    ESP_LOGI(TAG, "Log connection accepted, continuing");
*/
    lcd.puts("Mounting SDCard...\n");
    SDCard::PinCfg pins = { .clk = 14, .mosi = 13, .miso = 35, .cs = 15 };
    sdcard.init(HSPI_HOST, pins, "/sdcard");

    lcd.puts("Starting Player...\n");
    player.reset(new AudioPlayer(lcd));
    player->registerUrlHanlers(gHttpServer);
    player->playlist.load((char*)std::string(gPlaylist).c_str());
    otaNotifyCallback = []() {
        player->stop();
    };
    if (player->inputType() == AudioNode::kTypeHttpIn) {
        ESP_LOGI(TAG, "Player input set to HTTP stream");
        auto before = xPortGetFreeHeapSize();
        BluetoothStack::disableClassic();
        ESP_LOGW(TAG, "Releasing Bluetooth memory freed %d bytes of RAM", xPortGetFreeHeapSize() - before);
        player->playUrl("https://mediaserv38.live-streams.nl:18030/stream", "synthfm");
    } else if (player->inputType() == AudioNode::kTypeA2dpIn) {
        ESP_LOGI(TAG, "Player input set to Bluetooth A2DP sink");
        player->play();
    }
    ESP_LOGI(TAG, "player started");
}

static esp_err_t indexUrlHandler(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "<html><head /><body><h1 align='center'>NetPlayer HTTP inteface</h1><pre>Free heap memory: ");
    DynBuffer buf(128);

    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    buf.printf("%d\nchip type: %s\nnum cores: %d\nrunning at %d MHz\nsilicon revision: %d\n",
        xPortGetFreeHeapSize(), CONFIG_IDF_TARGET,
        chip_info.cores, currentCpuFreq(), chip_info.revision
    );
    httpd_resp_send_chunk(req, buf.buf(), buf.dataSize());
    buf.clear();

    buf.printf("radio: WiFi%s%s\nflash size: %dMB\nflash type: %s\n",
        (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "",
        spi_flash_get_chip_size() / (1024 * 1024),
        (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external"
    );
    httpd_resp_send_chunk(req, buf.buf(), buf.dataSize());
    buf.clear();
    buf.printf("ESP-IDF version: %d.%d.%d\n\nTasks:\n", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);
    httpd_resp_send_chunk(req, buf.buf(), buf.dataSize());

    std::string stats;
    taskList.update(&stats);
    httpd_resp_send_chunk(req, stats.c_str(), stats.size());
    httpd_resp_sendstr_chunk(req, "</pre></body></html>");
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

static esp_err_t changeInputUrlHandler(httpd_req_t *req)
{
    UrlParams params(req);
    auto type = params.strVal("mode");
    if (!type) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No mode param");
        return ESP_OK;
    }
    switch(type.str[0]) {
        case 'b':
            player->changeInput(AudioNode::kTypeA2dpIn);
            httpd_resp_sendstr(req, "Switched to Bluetooth A2DP sink");
            break;
        case 'h':
            player->changeInput(AudioNode::kTypeHttpIn);
            httpd_resp_sendstr(req, "Switched to HTTP client");
            break;
        default:
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid mode param");
            return ESP_OK;
    }
    ESP_LOGI(TAG, "Changed input node to type %d", player->nvs().readDefault<uint8_t>("inType", AudioNode::kTypeHttpIn));
    return ESP_OK;
}
static const httpd_uri_t changeInputUrl = {
    .uri       = "/inmode",
    .method    = HTTP_GET,
    .handler   = changeInputUrlHandler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = nullptr
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
    config.stack_size = 4096;
    config.max_uri_handlers = 16;
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
    httpd_register_uri_handler(gHttpServer, &changeInputUrl);
    httpFsRegisterHandlers(gHttpServer);
}


