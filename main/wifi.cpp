#include <esp_event_loop.h>
#include <esp_wifi.h>
#include <esp_log.h>

#include "wifi.hpp"
#include "utils.hpp"

static constexpr const char* TAG = "WiFi";

esp_err_t WifiClient::eventHandler(void *ctx, system_event_t *event)
{
    auto self = static_cast<WifiClient*>(ctx);
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
        ESP_LOGI(TAG, "Connected, got ip: " IPSTR,
                 IP2STR(&event->event_info.got_ip.ip_info.ip));
#else
        ESP_LOGI(TAG, "Connected, got ip: %s",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
#endif
        self->mRetryNum = 0;
        self->mEvents.setBits(kBitConnected);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        {
            self->mEvents.clearBits(kBitConnected);
            if (++self->mRetryNum >= kMaxConnectRetries) {
                ESP_LOGI(TAG, "Connect to AP failed, gave up");
                self->mEvents.setBits(kBitAborted);
                if (self->mConnRetryGaveUpHandler) {
                    self->mConnRetryGaveUpHandler();
                }
                break;
            }
            esp_wifi_connect();
            ESP_LOGI(TAG, "Connect to AP failed, retrying (%d)...", self->mRetryNum);
        }
    default:
        break;
    }
    return ESP_OK;
}

bool WifiClient::start(const char* ssid, const char* key, ConnGaveUpHandler onGaveUp)
{
    if (!ssid || !key) {
        return false;
    }
    auto ret = esp_event_loop_init(eventHandler, this);
    if (ret == ESP_OK) {
        tcpip_adapter_init();
    } else { // event loop already created
        esp_event_loop_set_cb(eventHandler, this);
    }

    mConnRetryGaveUpHandler = onGaveUp;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t staConfig;
    memset(&staConfig, 0, sizeof(staConfig));
    strncpy((char*)staConfig.sta.ssid, ssid, sizeof(staConfig.sta.ssid));
    strncpy((char*)staConfig.sta.password, key, sizeof(staConfig.sta.ssid));
    esp_err_t err;
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode error %s", esp_err_to_name(err));
        return err;
    }
    if ((err = esp_wifi_set_config(ESP_IF_WIFI_STA, &staConfig)) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config error %s", esp_err_to_name(err));
        return err;
    }

//    esp_wifi_set_ps(WIFI_PS_NONE);

    if ((err = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start error %s", esp_err_to_name(err));
        return err;
    }
    return true;
}
bool WifiClient::waitForConnect(int msTimeout)
{
    auto ret = mEvents.waitForOneNoReset(kBitConnected|kBitAborted, msTimeout);
    return ((ret != 0) && ((ret & kBitAborted) == 0));
}

void WifiClient::stop()
{
    esp_wifi_stop();
}

WifiClient::~WifiClient()
{
    stop();
}
