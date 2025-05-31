/* Blink Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <esp_ota_ops.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <driver/spi_common.h>
#include <st7735.hpp>
#include <wifi.hpp>
#include <httpServer.hpp>
#include <memory>
#include <nvsSimple.hpp>
#include <nvs_flash.h>
#include <mdns.hpp>
#include <asyncCall.hpp>
#include "rtcSharedMem.hpp"

static constexpr ST7735Display::PinCfg lcdPins = {
    .spi = {
        .clk = GPIO_NUM_18,
        .mosi = GPIO_NUM_23,
        .cs = GPIO_NUM_5,
    },
    .dc = GPIO_NUM_33, // data/command
    .rst = GPIO_NUM_4
};
static const char *TAG = "main";

const char* kDefaultMdnsDomain = "netplayer";
const int kRxChunkBufSize = 8192;
const char kPercentCompleteSuffix[] = "% complete";
esp_ota_handle_t gOtaPreparedHandle; // 0 is an invalid value in the current implementation, but not documented
bool gOtaPreparedHandleValid = false;
ST7735Display lcd(VSPI_HOST, St7735Driver::k1_9inch320x170);

typedef St7735Driver::Coord Coord;
const Coord kLcdLogFirstLine = 30;
const Coord kLcdOperationTxtY = lcd.height() - 54;
const Coord kLcdProgressPercentTxtY = kLcdOperationTxtY + 31;
const Coord kLcdProgressPercentBarY = kLcdOperationTxtY + 26;
const Coord kLcdProgressPercentBarHeight = 2;
int lastDisplayedPercent = 0;

extern Font font_Camingo22;
NvsSimple nvs;
std::unique_ptr<WifiBase> wifi;
http::Server server;
MDns mdns;

void gotoPercentPos()
{
    static Coord x = (lcd.width() - lcd.textWidth(5 + sizeof(kPercentCompleteSuffix) - 1)) / 2;
    lcd.gotoXY(x, kLcdProgressPercentTxtY);
}

void lcdDrawProgressScreen(const char* msg, bool displayProgress)
{
    ESP_LOGW(TAG, "%s", msg);
    lcd.setFont(font_Camingo22);
    lcd.clear(0, kLcdOperationTxtY, lcd.width(), lcd.fontHeight());
    lcd.clear(0, kLcdProgressPercentTxtY, lcd.width(), lcd.fontHeight());
    lcd.clear(0, kLcdProgressPercentBarY, lcd.width(), kLcdProgressPercentBarHeight);
    lcd.gotoXY(0, kLcdOperationTxtY);
    lcd.setFgColor(Color565::WHITE);
    lcd.putsCentered(msg);
    if (!displayProgress) {
        return;
    }
    gotoPercentPos();
    lcd.puts("  0");
    lcd.puts(kPercentCompleteSuffix);
    lastDisplayedPercent = 0;
}

void lcdUpdateProgress(int pct)
{
    if (pct == lastDisplayedPercent) {
        return;
    }
    char num[5];
    toString(num, sizeof(num), fmtInt(pct, 0, 3));
    gotoPercentPos();
    lcd.setFgColor(Color565::WHITE);
    lcd.puts(num);
    lcd.setFgColor(Color565::GREEN);
    lcd.fillRect(0, kLcdProgressPercentBarY, lcd.width() * pct / 100, kLcdProgressPercentBarHeight);
}
const char* setBootPartition(const esp_partition_t* appPartition)
{
    const char* kFuncName = "setBootPartition";
    if (!appPartition) {
        appPartition = esp_ota_get_next_update_partition(nullptr);
    }
    if (!appPartition) {
        const char* kMsg = "No app partition found";
        ESP_LOGE(TAG, "%s: %s", kFuncName, kMsg);
        return kMsg;
    }
    ESP_LOGW(TAG, "Setting boot partition to %s", appPartition->label);
    auto err = esp_ota_set_boot_partition(appPartition);
    if (err != ESP_OK) {
        const char* kMsg = "Error setting boot partition";
        ESP_LOGE(TAG, "%s: %s: %s", kFuncName, kMsg, esp_err_to_name(err));
        return kMsg;
    }
    return nullptr;
}
esp_err_t openAndEraseOtaPartition(esp_ota_handle_t& otaHandle, size_t imgSize)
{
    ElapsedTimer timer;
    auto partition = esp_ota_get_next_update_partition(NULL);
    lcdDrawProgressScreen("Erasing partition...", false);
    esp_err_t err = esp_ota_begin(partition, imgSize, &otaHandle);
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_LOGW(TAG, "Invalid OTA state of running app, trying to set it");
        esp_ota_mark_app_valid_cancel_rollback();
        err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &otaHandle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error %s after attempting to fix OTA state of running app, aborting OTA", esp_err_to_name(err));
            return err;
        }
    }
    else if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin returned error %s, aborting OTA", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "Erased partition '%s' size %ld KB, at offset 0x%lx",
        partition->label, partition->size / 1024, partition->address);
    ESP_LOGW(TAG, "Erase took %.1f seconds", (float)timer.usElapsed() / 1000000);
    return ESP_OK;
}
/* Receive .bin file */
esp_err_t httpOta(httpd_req_t *req)
{
    UrlParams params(req);
    int contentLen = req->content_len; // need an int instead of size_t for arithmetic operations
    ESP_LOGW(TAG, "OTA request received, image size: %d", contentLen);
    esp_ota_handle_t otaHandle;
    if (gOtaPreparedHandleValid) {
        otaHandle = gOtaPreparedHandle;
        gOtaPreparedHandleValid = false;
    } else {
        esp_err_t err = openAndEraseOtaPartition(otaHandle, contentLen);
        if (err != ESP_OK) {
            char msg[128];
            snprintf(msg, 127, "esp_ota_begin() error %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
            return ESP_FAIL;
        }
    }
    lcdDrawProgressScreen("Flashing firmware...", true);
    char otaBuf[kRxChunkBufSize];
    int totalRx = 0;
    ESP_LOGI(TAG, "Receiving and flashing image...");
    ElapsedTimer timer;
    while(totalRx < contentLen) {
        int recvLen = httpd_req_recv(req, otaBuf, std::min(contentLen - totalRx, kRxChunkBufSize));
        if (recvLen == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Receive timeout, retrying");
            continue;
        }
        else if (recvLen < 0) {
            ESP_LOGE(TAG, "OTA receive error %d, aborting", recvLen);
            return ESP_FAIL;
        }
        else if (recvLen == 0) {
            ESP_LOGE(TAG, "OTA receive returned zero bytes, aborting");
            return ESP_FAIL;
        }
        totalRx += recvLen;
        esp_ota_write(otaHandle, otaBuf, recvLen);
        lcdUpdateProgress((totalRx * 100 + (contentLen >> 1)) / contentLen);
    }
    ESP_LOGI(TAG, "Flash completed in %.1f seconds", (float)timer.usElapsed() / 1000000);

    auto err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end error: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_end error");
        return ESP_FAIL;
    }
    auto reboot = params.intVal("reboot", 1);
    if (reboot) {
        // Reboot asynchronously, after we return the http response
        asyncCall([]() {
            ESP_LOGI(TAG, "Restarting system...");
            esp_restart();
        }, 10);
    }
    httpd_resp_sendstr(req, "OTA update successful");
    return ESP_OK;
}

esp_err_t httpReboot(httpd_req_t *req)
{
    UrlParams params(req);
    auto toRecovery = params.intVal("recovery", 0);
    if (toRecovery) {
        auto errMsg = setBootPartition(esp_ota_get_running_partition());
        if (errMsg) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errMsg);
            return ESP_FAIL;
        }
    }
    const char* msg = toRecovery ? "Rebooting to recovery..." : "Rebooting to app...";
    ESP_LOGW(TAG, "%s", msg);
    asyncCall([msg]() {
        esp_restart();
    }, 500000);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

esp_err_t httpSetWifi(httpd_req_t* req)
{
    UrlParams params(req);
    auto ssid = params.strVal("ssid");
    auto pass = params.strVal("pass");
    if (!ssid || !pass) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid and/or pass URL param");
        return ESP_FAIL;
    }
    const char msg[] = "Error writing to NVS: ";
    auto err = nvs.setString("wifi.ssid", ssid.str);
    if (err != ESP_OK) {
        return http::sendEspError(req, HTTPD_500_INTERNAL_SERVER_ERROR, err, msg, sizeof(msg)-1);
    }
    err = nvs.setString("wifi.pass", pass.str);
    if (err != ESP_OK) {
        return http::sendEspError(req, HTTPD_500_INTERNAL_SERVER_ERROR, err, msg, sizeof(msg)-1);
    }
    httpd_resp_sendstr(req, "WiFi credentials updated");
    return ESP_OK;
}
esp_err_t httpNvsClear(httpd_req_t* req)
{
    nvs.close();
    auto err = nvs_flash_erase();
    if (err != ESP_OK) {
        http::sendEspError(req, HTTPD_500_INTERNAL_SERVER_ERROR, err, "Error erasing NVS partition");
        return ESP_FAIL;
    }
    err = nvs.init("aplayer", false);
    if (err != ESP_OK) {
        http::sendEspError(req, HTTPD_500_INTERNAL_SERVER_ERROR, err, "Error mounting NVS after erase");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "NVS partition erased");
    return ESP_OK;
}
esp_err_t httpIsRecovery(httpd_req_t* req)
{
    httpd_resp_sendstr(req, "yes");
    return ESP_OK;
}
const char* connectToWiFi()
{
    auto err = nvs.init("aplayer", false);
    if (err != ESP_OK) {
        return "Error mounting NVS storage";
    }
    auto ssid = nvs.getString("wifi.ssid");
    if (!ssid) {
        return "No WiFi ssid in config";
    }
    auto pass = nvs.getString("wifi.pass");
    if (!pass) {
        return "No WiFi password in config";
    }
    auto ySave = lcd.cursorY;
    lcd.puts("Connecting to WiFi...\n");
    wifi.reset(new WifiClient);
    static_cast<WifiClient*>(wifi.get())->start(ssid.get(), pass.get());
    bool success = wifi->waitForConnect(20000);
    lcd.clear(0, ySave, lcd.width(), lcd.fontHeight());
    lcd.gotoXY(0, ySave);
    if (!success) {
        return "Unable to connect";
    }
    return nullptr;
}
bool checkHandleRebootFlags()
{
    uint32_t flags = recoveryReadAndInvalidateFlags();

    if (esp_reset_reason() != ESP_RST_SW) {
        return false;
    }
    if (flags == kRecoveryFlagsInvalid) {
        ESP_LOGI(TAG, "RTC flags: no flags found");
        return false;
    }
    ESP_LOGW(TAG, "Detected application-sent flags: 0x%lx", flags);
    if (flags & kRecoveryFlagEraseImmediately) {
        auto err = openAndEraseOtaPartition(gOtaPreparedHandle, OTA_SIZE_UNKNOWN);
        gOtaPreparedHandleValid = (err == ESP_OK);
        lcdDrawProgressScreen("Waiting for image...", false);
    }
    return true;
}
extern "C" void app_main(void)
{
    lcd.init(lcdPins);
    lcd.setFont(Font_7x11, 2);
    lcd.gotoXY(0, 0);
    lcd.setFgColor(Color565::YELLOW);
    lcd.putsCentered("Recovery mode");
    lcd.setFgColor(Color565::WHITE);
    setBootPartition(nullptr);

    checkHandleRebootFlags();

    lcd.setFont(Font_7x11);
    lcd.gotoXY(0, kLcdLogFirstLine);
    auto err = connectToWiFi();
    if (err) {
        lcd.setFgColor(Color565::RED);
        lcd.puts("WiFi error: ");
        lcd.puts(err);
        lcd.setFgColor(Color565::WHITE);
        lcd.newLine();
        lcd.cursorY += 2;
        lcd.puts("Starting AP\n");
        wifi.reset(new WifiAp);
        static_cast<WifiAp*>(wifi.get())->start("netplayer", "alexisthebest", 1);
        lcd.puts("ssid: netplayer, key: alexisthebest\n");
    }
    const char* mDnsDomain = nvs.handle() ? nvs.getString("mdnsDomain").get() : nullptr;
    if (!mDnsDomain) {
        mDnsDomain = kDefaultMdnsDomain;
    }
    mdns.start(mDnsDomain);
    server.start(80, nullptr, 10, kRxChunkBufSize + 4096);
    char msg[48];
    snprintf(msg, 47, "Listening on http://" IPSTR, IP2STR(&wifi->localIp()));
    lcd.putsCentered(msg);
    lcd.newLine();

    server.on("/ota", HTTP_POST, httpOta);
    server.on("/reboot", HTTP_GET, httpReboot);
    server.on("/wifi", HTTP_GET, httpSetWifi);
    server.on("/nvclear", HTTP_GET, httpNvsClear);
    server.on("/isrecovery", HTTP_GET, httpIsRecovery);
    nvs.registerHttpHandlers(server);
}
