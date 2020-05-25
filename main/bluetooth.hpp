#ifndef BLUETOOTH_STACK_HPP
#define BLUETOOTH_STACK_HPP

#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_avrc_api.h>

class BluetoothStack
{
protected:
    static BluetoothStack* gInstance;
    static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    static void avrcControllerCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
public:
    static BluetoothStack* instance() { return gInstance; }
    static bool startInClassicMode(const char* discoName);
    static void disable(esp_bt_mode_t mode);
    static void disableCompletely() { disable(ESP_BT_MODE_BTDM); }
    static void disableBLE() { disable(ESP_BT_MODE_BLE); }
    static void becomeDiscoverableAndConnectable();
};

#endif
