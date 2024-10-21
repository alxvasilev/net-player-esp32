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

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"

class BTKeyboard
{
  public:
    typedef void pid_handler(uint32_t code);

    const uint8_t KEY_CAPS_LOCK = 0x39;

    enum class KeyModifier : uint8_t { 
      L_CTRL = 0x01, L_SHIFT = 0x02, L_ALT = 0x04, L_META = 0x08, 
      R_CTRL = 0x10, R_SHIFT = 0x20, R_ALT = 0x40, R_META = 0x80 };

    const uint8_t  CTRL_MASK = ((uint8_t) KeyModifier::L_CTRL ) | ((uint8_t) KeyModifier::R_CTRL ); 
    const uint8_t SHIFT_MASK = ((uint8_t) KeyModifier::L_SHIFT) | ((uint8_t) KeyModifier::R_SHIFT);
    const uint8_t   ALT_MASK = ((uint8_t) KeyModifier::L_ALT  ) | ((uint8_t) KeyModifier::R_ALT  );
    const uint8_t  META_MASK = ((uint8_t) KeyModifier::L_META ) | ((uint8_t) KeyModifier::R_META );

    static const uint8_t MAX_KEY_COUNT = 3;
    struct KeyInfo {
      KeyModifier modifier;
      uint8_t     keys[MAX_KEY_COUNT];
    };

  protected:
    static constexpr char const * TAG = "BTKeyboard";
    static xSemaphoreHandle bt_hidh_cb_semaphore;
    static xSemaphoreHandle ble_hidh_cb_semaphore;
    esp_hidh_dev_t* mHidDevice = nullptr;
    xQueueHandle  event_queue;
    int8_t        battery_level = 0;
    bool          key_avail[MAX_KEY_COUNT];
    char          last_ch;
    TickType_t    repeat_period;
    pid_handler* pairing_handler = nullptr;
    bool          caps_lock = false;

    struct esp_hid_scan_result_t { // only BLE needed
      struct esp_hid_scan_result_t * next;
      esp_bd_addr_t       bda;
      const char        * name;
      int8_t              rssi;
      esp_hid_usage_t     usage;
      esp_hid_transport_t transport; //BT, BLE or USB
      
      struct {
          esp_ble_addr_type_t addr_type;
          uint16_t            appearance;
      } ble;
    };
    esp_hid_scan_result_t* ble_scan_results = nullptr;
    size_t num_ble_scan_results = 0;

    static void hidh_callback(void * handler_args, esp_event_base_t base, int32_t id, void * event_data);
    static void ble_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t * param);

    static const char * ble_addr_type_str(esp_ble_addr_type_t ble_addr_type);
    static const char * ble_gap_evt_str(uint8_t event);
    static const char * ble_key_type_str(esp_ble_key_type_t key_type);

    static const char *      ble_gap_evt_names[];
    static const char *    ble_addr_type_names[];

    static const char         shift_trans_dict[];

    void handle_ble_device_result(esp_ble_gap_cb_param_t * scan_rst);  
    void esp_hid_scan_results_free(esp_hid_scan_result_t * results);
    esp_hid_scan_result_t * find_scan_result(esp_bd_addr_t bda, esp_hid_scan_result_t * results);
    void add_ble_scan_result(esp_bd_addr_t       bda, 
                             esp_ble_addr_type_t addr_type, 
                             uint16_t            appearance, 
                             uint8_t           * name, 
                             uint8_t             name_len, 
                             int                 rssi);

    void print_uuid(esp_bt_uuid_t * uuid);
    inline void set_battery_level(uint8_t level) { battery_level = level; }
    void push_key(uint8_t * keys, uint8_t size);
  public:
    BTKeyboard() {}

    bool setup(pid_handler * handler = nullptr);
    esp_err_t start_ble_scan(int seconds = 5);
    inline uint8_t get_battery_level() { return battery_level; }
    bool openBtHidDevice(const uint8_t* addr, int bleAddrType=-1);
    inline bool wait_for_low_event(KeyInfo & inf, TickType_t duration = portMAX_DELAY) {  
      return xQueueReceive(event_queue, &inf, duration); 
    }

    char wait_for_ascii_char(bool forever = true);
    inline char get_ascii_char() { return wait_for_ascii_char(false); }
};
