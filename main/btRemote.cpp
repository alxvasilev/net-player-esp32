#include "btRemote.hpp"
#include <cstring>
#include "utils.hpp"
#include "magic_enum.hpp"
#include "bluetooth.hpp"

// uncomment to print all devices that were seen during a scan
#define GAP_DBG_PRINTF(...) printf(__VA_ARGS__)
static constexpr const char* TAG = "btrmt";

bool BtRemote::init()
{
#ifdef CONFIG_BT_BLE_ENABLED
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
#endif // BLE
    esp_hidh_config_t config = {
        .callback = &sHidhCallback,
        .event_stack_size = 2400,
        .callback_arg = nullptr
    };
    MY_ESP_ERRCHECK(esp_hidh_init(&config), TAG, "init hidh", return false);
    return true;
}

bool BtRemote::openBtHidDevice(const uint8_t* addr, int bleAddrType)
{
    if (mHidDevice) {
        ESP_LOGE(TAG, "Already have open device");
        return false;
    }
    auto trans = bleAddrType < 0 ? ESP_HID_TRANSPORT_BT : ESP_HID_TRANSPORT_BLE;
    printf("before hid dev open\n");
    mHidDevice = esp_hidh_dev_open((uint8_t*)addr, trans, bleAddrType);
    printf("after hid dev open\n");
    return mHidDevice != nullptr;
}

void BtRemote::sHidhCallback(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
  esp_hidh_event_t        event = (esp_hidh_event_t) id;
  esp_hidh_event_data_t* param = (esp_hidh_event_data_t*) event_data;

  switch (event) {
    case ESP_HIDH_OPEN_EVENT: 
    // { // Code for ESP-IDF 4.3.1
    //   const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
    //   ESP_LOGV(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
    //   esp_hidh_dev_dump(param->open.dev, stdout);
    //   break;
    // }
    { // Code for ESP-IDF 4.4
      if (param->open.status == ESP_OK) {
        const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
        ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
        esp_hidh_dev_dump(param->open.dev, stdout);
      }
      else {
        ESP_LOGE(TAG, " OPEN failed with status %d", param->open.status);
      }
      break;
    }
    case ESP_HIDH_BATTERY_EVENT: {
      const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
      ESP_LOGI(TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda), param->battery.level);
      break;
    }
    case ESP_HIDH_INPUT_EVENT: {
      const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
      ESP_LOGI(TAG, ESP_BD_ADDR_STR " INPUT: %8s, MAP: %2u, ID: %3u, Len: %d, Data:",
                    ESP_BD_ADDR_HEX(bda), 
                    esp_hid_usage_str(param->input.usage), 
                    param->input.map_index, 
                    param->input.report_id, 
                    param->input.length);
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->input.data, param->input.length, ESP_LOG_DEBUG);
      break;
    }
    case ESP_HIDH_FEATURE_EVENT:  {
      const uint8_t *bda = esp_hidh_dev_bda_get(param->feature.dev);
      ESP_LOGI(TAG, ESP_BD_ADDR_STR " FEATURE: %8s, MAP: %2u, ID: %3u, Len: %d",
                    ESP_BD_ADDR_HEX(bda),
                    esp_hid_usage_str(param->feature.usage), 
                    param->feature.map_index, 
                    param->feature.report_id,
                    param->feature.length);
      ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->feature.data, param->feature.length, ESP_LOG_DEBUG);
      break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
      const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
      ESP_LOGI(TAG, ESP_BD_ADDR_STR " CLOSE: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->close.dev));
      break;
    }
    default:
      ESP_LOGI(TAG, "HID EVENT: %d", event);
      break;
  }
}
