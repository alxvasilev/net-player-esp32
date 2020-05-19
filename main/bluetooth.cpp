#include "esp_system.h"
#include "esp_log.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"
#include "bluetooth.hpp"

static const char* TAG = "btstack";
BluetoothStack* BluetoothStack::gInstance = nullptr;

void BluetoothStack::gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "authentication success: %s", param->auth_cmpl.device_name);
            esp_log_buffer_hex(TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        } else {
            ESP_LOGE(TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
        }
        break;
    }

#if (CONFIG_BT_SSP_ENABLED == true)
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_CFM_REQ_EVT Please compare the numeric value: %d", param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_NOTIF_EVT passkey:%d", param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "ESP_BT_GAP_KEY_REQ_EVT Please enter passkey!");
        break;
#endif
    default: {
        ESP_LOGI(TAG, "GAP event: %d", event);
        break;
    }
    }
    return;
}
void BluetoothStack::avrcControllerCallback(esp_avrc_ct_cb_event_t event,
                                            esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(TAG, "remote control cb event: %d", event);
}

bool BluetoothStack::startInClassicMode(const char* discoName)
{
    if (gInstance) {
        ESP_LOGE(TAG, "Bluetooth alreay started");
        return false;
    }
//    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_err_t err;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(err));
        return false;
    }

    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable controller failed: %s\n", __func__, esp_err_to_name(err));
        return false;
    }

    if ((err = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return false;
    }

    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "%s enable bluedroid failed: %s\n", __func__, esp_err_to_name(err));
        return false;
    }
    /* set up device name */
    esp_bt_dev_set_device_name(discoName);

    esp_bt_gap_register_callback(gapCallback);

    /* initialize AVRCP controller */
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(avrcControllerCallback);

    /* set discoverable and connectable mode, wait to be connected */
//    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    return true;
}

void BluetoothStack::disable(esp_bt_mode_t mode)
{
    if (gInstance) {
        delete gInstance;
        gInstance = nullptr;
    }
    esp_bluedroid_disable(); // Error: Bluedroid already disabled
    esp_bluedroid_deinit();  // Error: Bluedroid already deinitialized
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_mem_release(mode);
}
