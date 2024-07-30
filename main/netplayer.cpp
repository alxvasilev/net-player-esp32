#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_http_server.h>
#include <esp_spiffs.h>
#include <sys/param.h>
#include <string>
#include <algorithm> // for sorting task list
#include <memory>
#include <utils.hpp>
#include <netLogger.hpp>
#include <httpFile.hpp>
#include <wifi.hpp>
#include <taskList.hpp>
#include <st7735.hpp>
#include <sdcard.hpp>
#include <bluetooth.hpp>
#include <httpServer.hpp>
#include <nvsSimple.hpp>
#include <mdns.hpp>
#include <new>
#include "../recovery/main/rtcSharedMem.hpp"
#include "audioPlayer.hpp"

#define DEV_MODE 1

static constexpr gpio_num_t kPinButton = GPIO_NUM_27;
static constexpr gpio_num_t kPinLed = GPIO_NUM_2;
static constexpr St7735Driver::PinCfg lcdPins = {
    {
        .clk = GPIO_NUM_18,
        .mosi = GPIO_NUM_23,
        .cs = GPIO_NUM_5,
    },
    .dc = GPIO_NUM_33, // data/command
    .rst = GPIO_NUM_4
};

static const char *TAG = "netplay";
http::Server gHttpServer;
std::unique_ptr<AudioPlayer> player;
std::unique_ptr<WifiBase> wifi;
bool gIsAp = false;
TaskList taskList;
ST7735Display lcd(VSPI_HOST);
NvsSimple nvsSimple;
SDCard sdcard;
NetLogger netLogger(false);
MDns mdns;

void startWebserver(bool isAp=false);

void configGpios()
{
    gpio_pad_select_gpio(kPinButton);
    gpio_set_direction(kPinButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinButton);

    gpio_pad_select_gpio(kPinLed);
    gpio_set_direction(kPinLed, GPIO_MODE_OUTPUT);
}

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
void connectToWifi(bool forceAP = false)
{
    if (forceAP) {
        goto startAp;
    }
    {
        auto ssid = nvsSimple.getString("wifi.ssid");
        auto pass = nvsSimple.getString("wifi.pass");
        if (!ssid || !pass) {
            ESP_LOGW(TAG, "No valid WiFi credentials in NVS, starting access point...");
            goto startAp;
        }
        lcd.puts("Connecting to WiFi...");
        wifi.reset(new WifiClient);
        static_cast<WifiClient*>(wifi.get())->start(ssid.get(), pass.get());
        if (wifi->waitForConnect(20000)) {
            gIsAp = false;
            lcd.puts("success\n");
            goto connectSuccess;
        }
        lcd.puts("timeout\n");
    }
startAp:
    gIsAp = true;
    lcd.puts("Starting Wifi AP");
    lcd.puts("ssid: netplayer\n");
    lcd.puts("key: alexisthebest\n");
    wifi.reset(new WifiAp);
    static_cast<WifiAp*>(wifi.get())->start("netplayer", "alexisthebest", 1);
connectSuccess:
    char localIp[17];
    snprintf(localIp, 17, IPSTR, IP2STR(&wifi->localIp()));
    lcd.puts("Local IP is ");
    lcd.puts(localIp);
    lcd.newLine();
}
void startMdns()
{
    mdns.start(AudioPlayer::mdnsName());
}
void* operator new(size_t size)
{
    auto mem = utils::mallocTrySpiram(size);
    return mem;
}
extern "C" void* my_malloc(size_t size)
{
    //printf("my_malloc(%zu)\n", size);
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
extern "C" void* my_realloc(void* ptr, size_t size)
{
    //printf("my_realloc(%zu): isSpi: %d\n", size, utils::isInSpiRam(ptr));
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}
extern "C" void* my_calloc(size_t num, size_t size)
{
    //printf("my_calloc(%zu, %zu)\n", num, size);
    return heap_caps_calloc(num, size, MALLOC_CAP_SPIRAM);
}

extern "C" void app_main(void)
{
//  esp_log_level_set("*", ESP_LOG_DEBUG);
    utils::detectSpiRam();
    configGpios();

    lcd.init(320, 240, lcdPins);
    lcd.dmaEnable(2, false);
    lcd.setFont(Font_7x11);
    lcd.puts("Mounting NVS...\n");
    /* Initialize NVS — it is used to store PHY calibration data */
    nvsSimple.init("aplayer", true);
    lcd.puts("Mounting SPIFFS...\n");
    mountSpiffs();
    connectToWifi(!gpio_get_level(kPinButton));
    startMdns();
    lcd.puts("Starting webserver...\n");
    startWebserver();
#ifdef DEV_MODE
        lcd.puts("Waiting dev http request\n");
        msSleep(1000);
#endif
//===
    lcd.puts("Mounting SDCard...\n");
    SDCard::PinCfg pins = { .clk = 14, .mosi = 13, .miso = 35, .cs = 15 };
    sdcard.init(HSPI_HOST, pins, "/sdcard");
//====
    lcd.puts("Starting Player...\n");
    player.reset(new AudioPlayer(lcd, gHttpServer));
    MutexLocker locker(player->mutex);
    if (player->inputType() == AudioNode::kTypeHttpIn) {
        ESP_LOGI(TAG, "Player input set to HTTP stream");
        auto before = xPortGetFreeHeapSize();
        BluetoothStack::disableCompletely();
        ESP_LOGW(TAG, "Releasing all Bluetooth memory freed %d bytes of RAM", xPortGetFreeHeapSize() - before);
        player->playStation(nullptr);
    }
    else if (player->inputType() == AudioNode::kTypeA2dpIn) {
        ESP_LOGI(TAG, "Player input set to Bluetooth A2DP sink");
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
    DynBuffer buf(200);
    /* Print chip information */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    buf.printf("%zu of %zu (largest free blk: %zu)\nFree external RAM: ",
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (utils::haveSpiRam()) {
        buf.printf("%zu of %zu (largest free blk: %zu)\n",
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
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
const char* setBootFromRecovery()
{
    auto partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory");
    if (!partition) {
        return "Could not find recovery partition";
    }
    auto err = esp_ota_set_boot_partition(partition);
    if (err == ESP_OK) {
        return nullptr;
    }
    return esp_err_to_name(err);
}
esp_err_t httpReboot(httpd_req_t* req)
{
    UrlParams params(req);
    auto toRecovery = params.intVal("recovery", 0);
    if (toRecovery) {
        auto err = setBootFromRecovery();
        if (err) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err);
            return ESP_FAIL;
        }
        int flags = params.intVal("flags", 0);
        if (flags) {
            recoveryWriteFlags(flags);
            ESP_LOGW(TAG, "Sending flags to recovery: 0x%X", flags);
        }
    }
    httpd_resp_sendstr(req, "ok");
    ESP_LOGI(TAG, "Rebooting%s...", toRecovery ? " to recovery" : "");
    asyncCall([]() {
        esp_restart();
    }, 400000);
    return ESP_OK;
}
void startWebserver(bool isAp)
{
    ESP_LOGI(TAG, "Starting server on port 80...");
    auto err = gHttpServer.start(80, nullptr, 28, 4096);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error %s starting server", esp_err_to_name(err));
        return;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    netLogger.registerWithHttpServer(gHttpServer.handle(), "/log");
    gHttpServer.on("/reboot", HTTP_GET, httpReboot);
    gHttpServer.on("/", HTTP_GET, indexUrlHandler);
    httpFsRegisterHandlers(gHttpServer.handle());
}
