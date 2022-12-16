#include <string.h>
//#include <esp32/spiram.h>
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
#include "taskList.hpp"
#include <st7735.hpp>
#include "sdcard.hpp"
#include "bluetooth.hpp"
#include <new>

static constexpr gpio_num_t kPinButton = GPIO_NUM_27;
static constexpr gpio_num_t kPinRollbackButton = GPIO_NUM_32;
static constexpr gpio_num_t kPinLed = GPIO_NUM_2;

static const char *TAG = "netplay";

AudioPlayer::HttpServerInfo gHttpServer;
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
         ESP_LOGW(TAG, "SPIFFS partition size: total: %d, used: %d", total, used);
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

void* operator new(size_t size)
{
    auto mem = utils::mallocTrySpiram(size);
    printf("operator new(%zu): isSpi: %d\n", size, utils::isInSpiRam(mem));
    return mem;
}
extern "C" void* my_malloc(size_t size)
{
    printf("my_malloc(%zu)\n", size);
    return utils::mallocTrySpiram(size);
}
extern "C" void* my_realloc(void* ptr, size_t size)
{
    printf("my_realloc(%zu): isSpi: %d\n", size, utils::isInSpiRam(ptr));
    return utils::reallocTrySpiram(ptr, size);
}

extern "C" void app_main(void)
{
//  esp_log_level_set("*", ESP_LOG_DEBUG);
    utils::detectSpiRam();
    configGpios();

    rollbackCheckUserForced();
    rollbackConfirmAppIsWorking();

    lcd.init(320, 240, lcdPins);
    lcd.setFont(Font_7x11);
    lcd.puts("Mounting storage...\n");
    initNvs();
    mountSpiffs();

    if (!gpio_get_level(kPinButton)) {
        ESP_LOGW(TAG, "Started as WiFi access point");
        wifi.reset(new WifiAp);
        static_cast<WifiAp*>(wifi.get())->start("netplayer", "alexisthebest", 1);
        startWebserver(true);
        return;
    }

    //== WIFI
    lcd.puts("Connecting to WiFi...\n");
    wifi.reset(new WifiClient);
    static_cast<WifiClient*>(wifi.get())->start(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD);
    if (!wifi->waitForConnect(20000)) {
        lcd.puts("...timed out, starting AP\n");
        lcd.puts("ssid: netplayer\n");
        lcd.puts("key: alexisthebest\n");
        wifi.reset(new WifiAp);
        static_cast<WifiAp*>(wifi.get())->start("netplayer", "alexisthebest", 1);
    }
    char localIp[17];
    snprintf(localIp, 17, IPSTR, IP2STR(&wifi->localIp()));
    lcd.puts("Local IP is ");
    lcd.puts(localIp);
    lcd.newLine();
 //====
    lcd.puts("Starting webserver...\n");
    startWebserver();    
//===

    lcd.puts("Waiting log conn...\n");
    auto ret = netLogger.waitForLogConnection(4);
    if (gOtaInProgress) {
        lcd.puts("OTA Update in progress...\n");
        return;
    } else {
        lcd.puts("OTA NOT in progress\n");
    }

    ESP_LOGI(TAG, "%s, continuing", ret ? "Log connection accepted" : "Timeout, continuing");
//===
    lcd.puts("Mounting SDCard...\n");
    SDCard::PinCfg pins = { .clk = 14, .mosi = 13, .miso = 35, .cs = 15 };
    sdcard.init(HSPI_HOST, pins, "/sdcard");
//====
    lcd.puts("Starting Player...\n");
    player.reset(new AudioPlayer(lcd, gHttpServer));
    MutexLocker locker(player->mutex);
    otaNotifyCallback = []() {
        MutexLocker locker(player->mutex);
        player->stop();
    };
    if (player->inputType() == AudioNode::kTypeHttpIn) {
        ESP_LOGI(TAG, "Player input set to HTTP stream");
        auto before = xPortGetFreeHeapSize();
        BluetoothStack::disableCompletely();
        ESP_LOGW(TAG, "Releasing all Bluetooth memory freed %d bytes of RAM", xPortGetFreeHeapSize() - before);
        player->playStation(nullptr);
    }
    else if (player->inputType() == AudioNode::kTypeA2dpIn) {
        ESP_LOGI(TAG, "Player input set to Bluetooth A2DP sink");
        //===
        auto before = xPortGetFreeHeapSize();
        BluetoothStack::disableBLE();
        ESP_LOGW(TAG, "Releasing Bluetooth BLE memory freed %d bytes of RAM", xPortGetFreeHeapSize() - before);
        player->play();
        /*
                BluetoothStack::start(ESP_BT_MODE_BLE, "test");

                BluetoothStack::discoverDevices([](BluetoothStack::DeviceList& devices) {
                    for (auto& item: devices) {
                        ESP_LOGI(TAG, "%s(%s): class: %x, rssi: %d", item.second.name.c_str(),
                                 item.first.toString().c_str(), item.second.devClass, item.second.rssi);
                    }
                });
        */
    }
    ESP_LOGI(TAG, "player started");
}

static esp_err_t indexUrlHandler(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req,
        "<html><head /><body><h1 align='center'>NetPlayer HTTP inteface</h1><pre>Free internal RAM: ");
    DynBuffer buf(128);
    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    buf.printf("%zu of %zu\nFree external RAM: ",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    if (utils::haveSpiRam()) {
        buf.printf("%zu of %zu\n",
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    } else {
        buf.printf("Not available\n");
    }
    buf.printf("chip type: %s\nnum cores: %d\nrunning at %d MHz\nsilicon revision: %d\n",
        CONFIG_IDF_TARGET, chip_info.cores, utils::currentCpuFreq(), chip_info.revision
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


void stopWebserver() {
    /* Stop the web server */
    if (!gHttpServer.server) {
        return;
    }
    netLogger.unregisterWithHttpServer("/log");
    httpd_stop(gHttpServer.server);
    gHttpServer.server = nullptr;
}

void startWebserver(bool isAp)
{
    if (gHttpServer.server) {
        stopWebserver();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_uri_handlers = 24;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&gHttpServer.server, &config) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting server!");
        return;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    netLogger.registerWithHttpServer(gHttpServer.server, "/log");
    httpd_register_uri_handler(gHttpServer.server, &otaUrlHandler);
    httpd_register_uri_handler(gHttpServer.server, &indexUrl);
    httpFsRegisterHandlers(gHttpServer.server);
}
