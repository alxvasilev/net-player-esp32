#include "esp_system.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_a2dp_api.h"
#include "esp_hidh.h"
#include <esp_heap_caps.h>
#include "bluetooth.hpp"
#include <cstring>
#include "buffer.hpp"
#include "utils.hpp"

static const char* TAG = "btstack";
BluetoothStack* BluetoothStack::gInstance = nullptr;
std::unique_ptr<BluetoothStack::Discovery> BluetoothStack::gDiscovery;

static char *bda2str(esp_bd_addr_t bda);
static char *uuid2str(esp_bt_uuid_t *uuid);
static bool get_name_from_eir(uint8_t *eir, char* bdname, uint8_t len);

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
    case ESP_BT_GAP_DISC_RES_EVT: {
        if (gDiscovery) {
            gDiscovery->getDeviceInfo(param);
        }
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Device discovery stopped");
            if (gDiscovery) {
                gDiscovery->discoCompleteCb(gDiscovery->devices);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Device discovery started");
        }
        break;
    }
    case ESP_BT_GAP_RMT_SRVCS_EVT: {
        if (param->rmt_srvcs.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Services for device %s found",  bda2str(param->rmt_srvcs.bda));
            for (int i = 0; i < param->rmt_srvcs.num_uuids; i++) {
                esp_bt_uuid_t *u = param->rmt_srvcs.uuid_list + i;
                ESP_LOGI(TAG, "--%s", uuid2str(u));
                // ESP_LOGI(GAP_TAG, "--%d", u->len);
            }
        } else {
            ESP_LOGI(TAG, "Services for device %s not found",  bda2str(param->rmt_srvcs.bda));
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
}
void BluetoothStack::avrcControllerCallback(esp_avrc_ct_cb_event_t event,
                                            esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(TAG, "remote control cb event: %d", event);
}

esp_hidh_dev_t* BluetoothStack::connectHidDeviceBtClassic(const uint8_t* addr)
{
    return esp_hidh_dev_open((uint8_t*)addr, ESP_HID_TRANSPORT_BT, 0);
}

esp_hidh_dev_t* BluetoothStack::connectHidDeviceBLE(const esp_bd_addr_t addr, esp_ble_addr_type_t addrType)
{
    return esp_hidh_dev_open((uint8_t*)addr, ESP_HID_TRANSPORT_BLE, addrType);
}

void BluetoothStack::hidhCallback(void *args, esp_event_base_t base, int32_t id, void* data)
{
    auto event = (esp_hidh_event_t)id;
    auto param = (esp_hidh_event_data_t*)data;
    switch (event) {
     case ESP_HIDH_OPEN_EVENT: {
         const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
         ESP_LOGI(TAG, ESP_BD_ADDR_STR " OPEN: %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->open.dev));
         esp_hidh_dev_dump(param->open.dev, stdout);

         char hex_buffer_1[4 * 16 + 1];
         sprintf(hex_buffer_1, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
         ESP_LOGI(TAG, "Device %s connected", hex_buffer_1);
         break;
     }
     case ESP_HIDH_BATTERY_EVENT: {
         const uint8_t *bda = esp_hidh_dev_bda_get(param->battery.dev);
         ESP_LOGI(TAG, ESP_BD_ADDR_STR " BATTERY: %d%%", ESP_BD_ADDR_HEX(bda), param->battery.level);
         break;
     }
     case ESP_HIDH_INPUT_EVENT: {
         const uint8_t *bda = esp_hidh_dev_bda_get(param->input.dev);
         char hex_buffer_2[4 * 16 + 1];
         sprintf(hex_buffer_2, ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(bda));
         DynBuffer buf(param->input.length * 2 + 1);
         buf.setDataSize(buf.capacity());
         binToHex(param->input.data, param->input.length, buf.buf());
         buf.buf()[buf.dataSize()-1] = 0; // null terminate
         ESP_LOGI(TAG, "HID event: %s", buf.buf());
         break;
     }
     case ESP_HIDH_FEATURE_EVENT: {
         const uint8_t *bda = esp_hidh_dev_bda_get(param->feature.dev);
         ESP_LOGI(TAG, ESP_BD_ADDR_STR " FEATURE: %8s, MAP: %2u, ID: %3u, Len: %d", ESP_BD_ADDR_HEX(bda), esp_hid_usage_str(param->feature.usage), param->feature.map_index, param->feature.report_id, param->feature.length);
         ESP_LOG_BUFFER_HEX(TAG, param->feature.data, param->feature.length);
         break;
     }
     case ESP_HIDH_CLOSE_EVENT: {
         const uint8_t *bda = esp_hidh_dev_bda_get(param->close.dev);
         ESP_LOGI(TAG, ESP_BD_ADDR_STR " CLOSE: '%s' %s", ESP_BD_ADDR_HEX(bda), esp_hidh_dev_name_get(param->close.dev), esp_hid_disconnect_reason_str(esp_hidh_dev_transport_get(param->close.dev), param->close.reason));
         //MUST call this function to free all allocated memory by this device
         esp_hidh_dev_free(param->close.dev);
         break;
     }
     default:
         ESP_LOGI(TAG, "HID EVENT: %d", event);
         break;
     }
}
void BluetoothStack::startHidHost()
{
    esp_hidh_config_t config = {
        .callback = hidhCallback,
//      .event_stack_size = 4096
    };
    ESP_ERROR_CHECK(esp_hidh_init(&config));
}
bool BluetoothStack::start(esp_bt_mode_t mode, const char* discoName)
{
    if (gInstance) {
        ESP_LOGE(TAG, "Bluetooth already started");
        return false;
    }

//    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_err_t err;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    cfg.mode = mode;
    if ((err = esp_bt_controller_init(&cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "%s initialize controller failed: %s\n", __func__, esp_err_to_name(err));
        return false;
    }

    if ((err = esp_bt_controller_enable(mode)) != ESP_OK) {
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

    return true;
}
void BluetoothStack::becomeDiscoverableAndConnectable()
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
#else
    // legacy API
    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
#endif

}
void BluetoothStack::discoverDevices(void(*cb)(DeviceList& devices))
{
    if (gDiscovery) {
        ESP_LOGE(TAG, "Discovery already in progress");
        return;
    }
    gDiscovery.reset(new Discovery);
    gDiscovery->discoCompleteCb = cb;
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
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
void BluetoothStack::Discovery::getDeviceInfo(esp_bt_gap_cb_param_t *param)
{
    Addr addr;
    memcpy(addr.data(), param->disc_res.bda, addr.size());
    if (devices.find(addr) != devices.end()) {
        return;
    }
    ESP_LOGI(TAG, "Device found: %s", bda2str(param->disc_res.bda));
    auto& info = devices[addr];
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        auto p = param->disc_res.prop + i;
        switch (p->type) {
            case ESP_BT_GAP_DEV_PROP_COD:
                info.devClass = *(uint32_t *)(p->val);
                ESP_LOGI(TAG, "--Class of Device: 0x%x", info.devClass);
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                info.rssi = *(int8_t *)(p->val);
                ESP_LOGI(TAG, "--RSSI: %d", info.rssi);
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                ESP_LOGI(TAG, "Device name: %.*s", p->len, (char*)p->val);
                if (info.name.empty()) {
                    info.name.assign((char*)p->val, p->len);
                }
                break;
            case ESP_BT_GAP_DEV_PROP_EIR: {
                char name[128];
                if (!get_name_from_eir((uint8_t*)p->val, name, sizeof(name))) {
                    strcpy(name, "(error)");
                }
                ESP_LOGI(TAG, "Device name from EIR: %s", name);
                if (info.name.empty()) {
                    info.name = name;
                }
                break;
            }
            default:
                ESP_LOGI(TAG, "Unknown property %d", p->type);
                break;
        }
    }
}
static char *bda2str(esp_bd_addr_t bda)
{
    static char str[18];
    if (bda == NULL) {
        return NULL;
    }

    uint8_t *p = bda;
    sprintf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
            p[0], p[1], p[2], p[3], p[4], p[5]);
    return str;
}
std::string BluetoothStack::Addr::toString() const
{
    std::string result;
    result.resize(18);
    auto p = data();
    snprintf(&result.front(), 18, "%02x:%02x:%02x:%02x:%02x:%02x",
        p[0], p[1], p[2], p[3], p[4], p[5]);
    return result;
}
static char *uuid2str(esp_bt_uuid_t *uuid)
{
    static char str[37];
    if (uuid == NULL || str == NULL) {
        return NULL;
    }

    if (uuid->len == 2) {
        sprintf(str, "%04x", uuid->uuid.uuid16);
    } else if (uuid->len == 4) {
        sprintf(str, "%08x", uuid->uuid.uuid32);
    } else if (uuid->len == 16) {
        uint8_t *p = uuid->uuid.uuid128;
        sprintf(str, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                p[15], p[14], p[13], p[12], p[11], p[10], p[9], p[8],
                p[7], p[6], p[5], p[4], p[3], p[2], p[1], p[0]);
    } else {
        return NULL;
    }

    return str;
}

static bool get_name_from_eir(uint8_t *eir, char* bdname, uint8_t len)
{
    if (!eir) {
        return false;
    }

    uint8_t rmt_bdname_len = 0;
    auto rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &rmt_bdname_len);
    if (!rmt_bdname) {
        rmt_bdname = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &rmt_bdname_len);
    }

    if (!rmt_bdname) {
        return false;
    }
    if (rmt_bdname_len > len-1) {
        rmt_bdname_len = len-1;
    }
    memcpy(bdname, rmt_bdname, rmt_bdname_len);
    bdname[rmt_bdname_len] = '\0';
    return true;
}
