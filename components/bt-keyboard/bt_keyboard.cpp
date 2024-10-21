// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.
//
// -----
//
// Original code from the bluetooth/esp_hid_host example of ESP-IDF license:
//
// Copyright 2017-2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define __BT_KEYBOARD__ 1
#include "bt_keyboard.hpp"

#include <cstring>

#define SCAN 1

// uncomment to print all devices that were seen during a scan
#define GAP_DBG_PRINTF(...) printf(__VA_ARGS__)

#define SIZEOF_ARRAY(a) (sizeof(a) / sizeof(*a))

#define WAIT_BT_CB() xSemaphoreTake(bt_hidh_cb_semaphore, portMAX_DELAY)
#define SEND_BT_CB() xSemaphoreGive(bt_hidh_cb_semaphore)

#define WAIT_BLE_CB() xSemaphoreTake(ble_hidh_cb_semaphore, portMAX_DELAY)
#define SEND_BLE_CB() xSemaphoreGive(ble_hidh_cb_semaphore)

xSemaphoreHandle BTKeyboard::bt_hidh_cb_semaphore = nullptr;
xSemaphoreHandle BTKeyboard::ble_hidh_cb_semaphore = nullptr;

const char *      BTKeyboard::ble_gap_evt_names[] = { "ADV_DATA_SET_COMPLETE", "SCAN_RSP_DATA_SET_COMPLETE", "SCAN_PARAM_SET_COMPLETE", "SCAN_RESULT", "ADV_DATA_RAW_SET_COMPLETE", "SCAN_RSP_DATA_RAW_SET_COMPLETE", "ADV_START_COMPLETE", "SCAN_START_COMPLETE", "AUTH_CMPL", "KEY", "SEC_REQ", "PASSKEY_NOTIF", "PASSKEY_REQ", "OOB_REQ", "LOCAL_IR", "LOCAL_ER", "NC_REQ", "ADV_STOP_COMPLETE", "SCAN_STOP_COMPLETE", "SET_STATIC_RAND_ADDR", "UPDATE_CONN_PARAMS", "SET_PKT_LENGTH_COMPLETE", "SET_LOCAL_PRIVACY_COMPLETE", "REMOVE_BOND_DEV_COMPLETE", "CLEAR_BOND_DEV_COMPLETE", "GET_BOND_DEV_COMPLETE", "READ_RSSI_COMPLETE", "UPDATE_WHITELIST_COMPLETE" };
const char *    BTKeyboard::ble_addr_type_names[] = { "PUBLIC", "RANDOM", "RPA_PUBLIC", "RPA_RANDOM" };

const char BTKeyboard::shift_trans_dict[] = 
  "aAbBcCdDeEfFgGhHiIjJkKlLmMnNoOpPqQrRsStTuUvVwWxXyYzZ1!2@3#4$5%6^7&8*9(0)"
  "\r\r\033\033\b\b\t\t  -_=+[{]}\\|??;:'\"`~,<.>/?"
  "\200\200"                                            // CAPS LOC
  "\201\201\202\202\203\203\204\204\205\205\206\206"    // F1..F6
  "\207\207\210\210\211\211\212\212\213\213\214\214"    // F7..F12
  "\215\215\216\216\217\217"                            // PrintScreen ScrollLock Pause
  "\220\220\221\221\222\222\177\177"                    // Insert Home PageUp Delete
  "\223\223\224\224\225\225\226\226\227\227\230\230";   // End PageDown Right Left Dow Up

static BTKeyboard * bt_keyboard = nullptr;

const char * 
BTKeyboard::ble_addr_type_str(esp_ble_addr_type_t ble_addr_type)
{
  if (ble_addr_type > BLE_ADDR_TYPE_RPA_RANDOM) {
    return "UNKNOWN";
  }
  return ble_addr_type_names[ble_addr_type];
}

const char * 
BTKeyboard::ble_gap_evt_str(uint8_t event)
{
  if (event >= SIZEOF_ARRAY(ble_gap_evt_names)) {
    return "UNKNOWN";
  }
  return ble_gap_evt_names[event];
}

const char * 
BTKeyboard::ble_key_type_str(esp_ble_key_type_t key_type)
{
  const char *key_str = nullptr;
  switch (key_type) {
    case ESP_LE_KEY_NONE:
      key_str = "ESP_LE_KEY_NONE";
      break;
    case ESP_LE_KEY_PENC:
      key_str = "ESP_LE_KEY_PENC";
      break;
    case ESP_LE_KEY_PID:
      key_str = "ESP_LE_KEY_PID";
      break;
    case ESP_LE_KEY_PCSRK:
      key_str = "ESP_LE_KEY_PCSRK";
      break;
    case ESP_LE_KEY_PLK:
      key_str = "ESP_LE_KEY_PLK";
      break;
    case ESP_LE_KEY_LLK:
      key_str = "ESP_LE_KEY_LLK";
      break;
    case ESP_LE_KEY_LENC:
      key_str = "ESP_LE_KEY_LENC";
      break;
    case ESP_LE_KEY_LID:
      key_str = "ESP_LE_KEY_LID";
      break;
    case ESP_LE_KEY_LCSRK:
      key_str = "ESP_LE_KEY_LCSRK";
      break;
    default:
      key_str = "INVALID BLE KEY TYPE";
      break;
  }

  return key_str;
}

BTKeyboard::esp_hid_scan_result_t *
BTKeyboard::find_scan_result(esp_bd_addr_t bda, esp_hid_scan_result_t * results)
{
  esp_hid_scan_result_t *r = results;
  while (r) {
    if (memcmp(bda, r->bda, sizeof(esp_bd_addr_t)) == 0) {
      return r;
    }
    r = r->next;
  }
  return nullptr;
}

void 
BTKeyboard::add_ble_scan_result(esp_bd_addr_t       bda, 
                                esp_ble_addr_type_t addr_type, 
                                uint16_t            appearance, 
                                uint8_t           * name, 
                                uint8_t             name_len, 
                                int                 rssi)
{
  if (find_scan_result(bda, ble_scan_results)) {
    ESP_LOGW(TAG, "Result already exists!");
    return;
  }

  esp_hid_scan_result_t *r = (esp_hid_scan_result_t *)malloc(sizeof(esp_hid_scan_result_t));

  if (r == nullptr) {
    ESP_LOGE(TAG, "Malloc ble_hidh_scan_result_t failed!");
    return;
  }

  r->transport = ESP_HID_TRANSPORT_BLE;

  memcpy(r->bda, bda, sizeof(esp_bd_addr_t));

  r->ble.appearance = appearance;
  r->ble.addr_type  = addr_type;
  r->usage          = esp_hid_usage_from_appearance(appearance);
  r->rssi           = rssi;
  r->name           = nullptr;

  if (name_len && name) {
    char *name_s = (char *)malloc(name_len + 1);
    if (name_s == nullptr) {
      free(r);
      ESP_LOGE(TAG, "Malloc result name failed!");
      return;
    }
    memcpy(name_s, name, name_len);
    name_s[name_len] = 0;
    r->name = (const char *)name_s;
  }

  r->next = ble_scan_results;
  ble_scan_results = r;
  num_ble_scan_results++;
}

bool BTKeyboard::setup(pid_handler * handler)
{
  if (bt_keyboard) {
    ESP_LOGE(TAG, "Setup called more than once. Only one instance of BTKeyboard is allowed.");
    return false;
  }
  bt_keyboard = this;
  pairing_handler = handler;
  event_queue = xQueueCreate(10, sizeof(KeyInfo));

  bt_hidh_cb_semaphore = xSemaphoreCreateBinary();
  if (bt_hidh_cb_semaphore == nullptr) {
    ESP_LOGE(TAG, "xSemaphoreCreateMutex failed!");
    return false;
  }

  ble_hidh_cb_semaphore = xSemaphoreCreateBinary();
  if (ble_hidh_cb_semaphore == nullptr) {
    ESP_LOGE(TAG, "xSemaphoreCreateMutex failed!");
    vSemaphoreDelete(bt_hidh_cb_semaphore);
    bt_hidh_cb_semaphore = nullptr;
    return false;
  }

  // BLE GAP
  esp_err_t ret;
  if ((ret = esp_ble_gap_register_callback(ble_gap_event_handler))) {
    ESP_LOGE(TAG, "esp_ble_gap_register_callback failed: %d", ret);
    return false;
  }

  ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));
  esp_hidh_config_t config = {
    .callback = hidh_callback,
    .event_stack_size = 4*1024, // Required with ESP-IDF 4.4
    .callback_arg = nullptr   // idem
  };
  ESP_ERROR_CHECK(esp_hidh_init(&config));

  for (int i = 0; i < MAX_KEY_COUNT; i++) {
    key_avail[i] = true;
  }

  last_ch = 0;
  battery_level = -1;
  return true;
}

void
BTKeyboard::handle_ble_device_result(esp_ble_gap_cb_param_t * param)
{
  uint16_t uuid       = 0;
  uint16_t appearance = 0;
  char     name[64]   = "";

  uint8_t   uuid_len = 0;
  uint8_t * uuid_d   = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_16SRV_CMPL, &uuid_len);

  if (uuid_d != nullptr && uuid_len) {
    uuid = uuid_d[0] + (uuid_d[1] << 8);
  }

  uint8_t   appearance_len = 0;
  uint8_t * appearance_d   = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_APPEARANCE, &appearance_len);

  if (appearance_d != nullptr && appearance_len) {
    appearance = appearance_d[0] + (appearance_d[1] << 8);
  }

  uint8_t   adv_name_len = 0;
  uint8_t * adv_name     = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);

  if (adv_name == nullptr) {
    adv_name = esp_ble_resolve_adv_data(param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_SHORT, &adv_name_len);
  }

  if (adv_name != nullptr && adv_name_len) {
    memcpy(name, adv_name, adv_name_len);
    name[adv_name_len] = 0;
  }

  GAP_DBG_PRINTF("BLE: " ESP_BD_ADDR_STR ", ", ESP_BD_ADDR_HEX(param->scan_rst.bda));
  GAP_DBG_PRINTF("RSSI: %d, ",                 param->scan_rst.rssi);
  GAP_DBG_PRINTF("UUID: 0x%04x, ",             uuid);
  GAP_DBG_PRINTF("APPEARANCE: 0x%04x, ",       appearance);
  GAP_DBG_PRINTF("ADDR_TYPE: '%s'",            ble_addr_type_str(param->scan_rst.ble_addr_type));

  if (adv_name_len) {
    GAP_DBG_PRINTF(", NAME: '%s'", name);
  }
  GAP_DBG_PRINTF("\n");

  #if SCAN
    if (uuid == ESP_GATT_UUID_HID_SVC) {
      add_ble_scan_result(param->scan_rst.bda, 
                          param->scan_rst.ble_addr_type, 
                          appearance, adv_name, adv_name_len, 
                          param->scan_rst.rssi);
    }
  #endif
}

void BTKeyboard::ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param)
{
  switch (event) {

    // SCAN

    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
      ESP_LOGV(TAG, "BLE GAP EVENT SCAN_PARAM_SET_COMPLETE");
      SEND_BLE_CB();
      break;
    }
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      switch (param->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
          bt_keyboard->handle_ble_device_result(param);
          break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
          ESP_LOGV(TAG, "BLE GAP EVENT SCAN DONE: %d", param->scan_rst.num_resps);
          SEND_BLE_CB();
          break;
        default:
          break;
      }
      break;
    }
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
      ESP_LOGV(TAG, "BLE GAP EVENT SCAN CANCELED");
      break;
    }

    // ADVERTISEMENT

    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
      ESP_LOGV(TAG, "BLE GAP ADV_DATA_SET_COMPLETE");
      break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      ESP_LOGV(TAG, "BLE GAP ADV_START_COMPLETE");
      break;

    // AUTHENTICATION

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (!param->ble_security.auth_cmpl.success) {
        ESP_LOGE(TAG, "BLE GAP AUTH ERROR: 0x%x", param->ble_security.auth_cmpl.fail_reason);
      } 
      else {
        ESP_LOGV(TAG, "BLE GAP AUTH SUCCESS");
      }
      break;

    case ESP_GAP_BLE_KEY_EVT: //shows the ble key info share with peer device to the user.
      ESP_LOGV(TAG, "BLE GAP KEY type = %s", ble_key_type_str(param->ble_security.ble_key.key_type));
      break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: // ESP_IO_CAP_OUT
      // The app will receive this evt when the IO has Output capability and the peer device IO has Input capability.
      // Show the passkey number to the user to input it in the peer device.
      ESP_LOGV(TAG, "BLE GAP PASSKEY_NOTIF passkey:%d", param->ble_security.key_notif.passkey);
      if (bt_keyboard->pairing_handler != nullptr) (*bt_keyboard->pairing_handler)(param->ble_security.key_notif.passkey);
      break;

    case ESP_GAP_BLE_NC_REQ_EVT: // ESP_IO_CAP_IO
      // The app will receive this event when the IO has DisplayYesNO capability and the peer device IO also has DisplayYesNo capability.
      // show the passkey number to the user to confirm it with the number displayed by peer device.
      ESP_LOGV(TAG, "BLE GAP NC_REQ passkey:%d", param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT: // ESP_IO_CAP_IN
      // The app will receive this evt when the IO has Input capability and the peer device IO has Output capability.
      // See the passkey number on the peer device and send it back.
      ESP_LOGV(TAG, "BLE GAP PASSKEY_REQ");
      //esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, 1234);
      break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
      ESP_LOGV(TAG, "BLE GAP SEC_REQ");
      // Send the positive(true) security response to the peer device to accept the security request.
      // If not accept the security request, should send the security response with negative(false) accept value.
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;

    default:
      ESP_LOGV(TAG, "BLE GAP EVENT %s", ble_gap_evt_str(event));
      break;
  }
}

esp_err_t BTKeyboard::start_ble_scan(int seconds)
{
  if (num_ble_scan_results || ble_scan_results) {
      ESP_LOGE(TAG, "There are old scan results. Free them first!");
      return ESP_FAIL;
  }
  ESP_LOGV(TAG, "SCAN BLE...");

  static esp_ble_scan_params_t hid_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x50,
    .scan_window        = 0x30,
    .scan_duplicate     = BLE_SCAN_DUPLICATE_ENABLE,
  };

  esp_err_t ret = ESP_OK;
  if ((ret = esp_ble_gap_set_scan_params(&hid_scan_params)) != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_set_scan_params failed: %d", ret);
    return ret;
  }
  WAIT_BLE_CB();

  if ((ret = esp_ble_gap_start_scanning(seconds)) != ESP_OK) {
    ESP_LOGE(TAG, "esp_ble_gap_start_scanning failed: %d", ret);
    return ret;
  }
  return ret;
}
bool BTKeyboard::openBtHidDevice(const uint8_t* addr, int bleAddrType)
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

void BTKeyboard::hidh_callback(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
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
      bt_keyboard->set_battery_level(param->battery.level);
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
      bt_keyboard->push_key(param->input.data, param->input.length);
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

void 
BTKeyboard::push_key(uint8_t * keys, uint8_t size)
{
  KeyInfo inf;
  inf.modifier = (KeyModifier) keys[0];
  
  uint8_t max = (size > MAX_KEY_COUNT + 2) ? MAX_KEY_COUNT : size - 2;
  inf.keys[0] = inf.keys[1] = inf.keys[2] = 0;
  for (int i = 0; i < max; i++) {
    inf.keys[i] = keys[i + 2];
  }

  xQueueSend(event_queue, &inf, 0);
}

char
BTKeyboard::wait_for_ascii_char(bool forever)
{
  KeyInfo inf;

  while (true) {
    if (!wait_for_low_event(inf, (last_ch == 0) ? (forever ? portMAX_DELAY : 0) : repeat_period)) {
      repeat_period = pdMS_TO_TICKS(120);
      return last_ch;
    }

    int k = -1;
    for (int i = 0; i < MAX_KEY_COUNT; i++) {
      if ((k < 0) && key_avail[i]) k = i;
      key_avail[i] = inf.keys[i] == 0; 
    }

    if (k < 0) {
      continue;
    }

    char ch = inf.keys[k];

    if (ch >= 4) {
      if ((uint8_t) inf.modifier & CTRL_MASK) {
        if (ch < (3 + 26)) {
          repeat_period = pdMS_TO_TICKS(500);
          return last_ch = (ch - 3);
        }
      }
      else if (ch <= 0x52) {
        //ESP_LOGI(TAG, "Scan code: %d", ch);
        if (ch == KEY_CAPS_LOCK) caps_lock = !caps_lock;
        if ((uint8_t) inf.modifier & SHIFT_MASK) {
          if (caps_lock) {
            repeat_period = pdMS_TO_TICKS(500);
            return last_ch = shift_trans_dict[(ch - 4) << 1];
          }
          else {
            repeat_period = pdMS_TO_TICKS(500);
            return last_ch = shift_trans_dict[((ch - 4) << 1) + 1];
          }
        }
        else {
          if (caps_lock) {
            repeat_period = pdMS_TO_TICKS(500);
            return last_ch = shift_trans_dict[((ch - 4) << 1) + 1];
          }
          else {
            repeat_period = pdMS_TO_TICKS(500);
            return last_ch = shift_trans_dict[(ch - 4) << 1];
          }
        }
      }
    }

    last_ch = 0; 
  }
}
