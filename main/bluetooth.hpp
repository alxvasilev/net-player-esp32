#ifndef BLUETOOTH_STACK_HPP
#define BLUETOOTH_STACK_HPP

#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_avrc_api.h>
#include <string>
#include <map>
#include <memory>

class BluetoothStack
{
public:
    struct DeviceInfo {
        uint32_t devClass;
        std::string name;
        int rssi;
    };
    struct Addr: public std::array<uint8_t, ESP_BD_ADDR_LEN> {
        std::string toString() const;
    };
    typedef std::map<Addr, DeviceInfo> DeviceList;
protected:
    struct Discovery {
        DeviceList devices;
        void(*discoCompleteCb)(DeviceList&);
        void getDeviceInfo(esp_bt_gap_cb_param_t *param);
    };
    static BluetoothStack* gInstance;
    static std::unique_ptr<Discovery> gDiscovery;
    static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
    static void avrcControllerCallback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
public:
    static BluetoothStack* instance() { return gInstance; }
    static bool start(esp_bt_mode_t mode, const char* discoName);
    static void disable(esp_bt_mode_t mode);
    static void disableCompletely() { disable(ESP_BT_MODE_BTDM); }
    static void disableBLE() { disable(ESP_BT_MODE_BLE); }
    static void disableClassic() { disable(ESP_BT_MODE_CLASSIC_BT); }
    static void becomeDiscoverableAndConnectable();
    static void discoverDevices(void(*cb)(DeviceList&));
};

#endif
