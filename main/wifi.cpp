#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>

#include "wifi.hpp"
#include "utils.hpp"

static constexpr const char* TAG = "WiFi";

void WifiClient::wifiEventHandler(void* userp, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data)
{
    auto self = static_cast<WifiClient*>(userp);
    switch(event_id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
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
    default: break;
    }
}

void WifiClient::gotIpEventHandler(void* userp, esp_event_base_t event_base,
                                   int32_t event_id, void* event_data)
{
    myassert(event_id == IP_EVENT_STA_GOT_IP);
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    auto self = static_cast<WifiClient*>(userp);
    self->mLocalIp = event->ip_info.ip;
    ESP_LOGI(TAG, "Connected, got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    self->mRetryNum = 0;
    self->mEvents.setBits(kBitConnected);
}

bool WifiClient::start(const char* ssid, const char* key, ConnGaveUpHandler onGaveUp)
{
    if (!ssid || !key) {
        return false;
    }
    mConnRetryGaveUpHandler = onGaveUp;
    eventLoopCreateAndRegisterHandler(wifiEventHandler);
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &gotIpEventHandler, this));

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
bool WifiBase::waitForConnect(int msTimeout)
{
    auto ret = mEvents.waitForOneNoReset(kBitConnected|kBitAborted, msTimeout);
    return ((ret != 0) && ((ret & kBitAborted) == 0));
}

void WifiClient::stop()
{
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEventHandler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, gotIpEventHandler);
    esp_wifi_stop();
}

WifiClient::~WifiClient()
{
    stop();
}

void WifiAp::reconfigDhcpServer()
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
    ESP_LOGI("WiFiAP", "DHCP server started \n");
}

void WifiAp::apEventHandler(void* userp, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data)
{
    switch(event_id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        auto event = (wifi_event_ap_staconnected_t*)event_data;
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        auto event = (wifi_event_ap_stadisconnected_t*)event_data;
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->mac),
                 event->aid);
        break;
    }
    default:
        break;
    }
}
void WifiAp::start(const char* ssid, const char* key, uint8_t chan)
{
    eventLoopCreateAndRegisterHandler(apEventHandler);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    //ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

//  reconfigDhcpServer();

    // configure the wifi connection and start the interface
    wifi_config_t ap_config;
    auto& ap = ap_config.ap;
    strcpy((char*)ap.ssid, ssid);
    strcpy((char*)ap.password, key);
    ap.ssid_len = 0;
    ap.channel = chan;
    ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ssid_hidden = 0;
    ap.max_connection = 8;
    ap.beacon_interval = 400;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(20));
}

void WifiAp::stop()
{
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, apEventHandler);
    esp_wifi_stop();
}

WifiAp::~WifiAp()
{
    stop();
}

void WifiBase::eventLoopCreateAndRegisterHandler(esp_event_handler_t handler)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        ESP_ERROR_CHECK(esp_netif_init());
    }
    if (mIsAp) {
        esp_netif_create_default_wifi_ap();
    } else {
        esp_netif_create_default_wifi_sta();
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler, this);
}
