#ifndef BLUETOOTH_STACK_HPP
#define BLUETOOTH_STACK_HPP

#include <esp_bt.h>
#include <esp_gap_bt_api.h>
#include <esp_avrc_api.h>
#include <esp_event.h>
#include <string>
#include <map>
#include <memory>
#include <esp_hidh.h>

class BluetoothStack
{
public:
    struct DeviceInfo {
        uint32_t devClass;
        std::string name;
        int rssi;
    };
    struct Addr: public std::array<uint8_t, ESP_BD_ADDR_LEN> {
        typedef std::array<uint8_t, ESP_BD_ADDR_LEN> Base;
        Addr(const Base& arr): Base(arr) {}
        Addr() {}
        operator const esp_bd_addr_t& () const { return (const esp_bd_addr_t&)front(); }
        operator esp_bd_addr_t& () { return (esp_bd_addr_t&)front(); }
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
    static void hidhCallback(void *args, esp_event_base_t base, int32_t id, void* data);

public:
    static BluetoothStack* instance() { return gInstance; }
    static void startHidHost();
    static esp_hidh_dev_t* connectHidDeviceBtClassic(const uint8_t* addr);
    static esp_hidh_dev_t* connectHidDeviceBLE(const esp_bd_addr_t addr, esp_ble_addr_type_t addrType);

    static bool start(esp_bt_mode_t mode, const char* discoName);
    static void disable(esp_bt_mode_t mode);
    static void disableCompletely() { disable(ESP_BT_MODE_BTDM); }
    static void disableBLE() { disable(ESP_BT_MODE_BLE); }
    static void disableClassic() { disable(ESP_BT_MODE_CLASSIC_BT); }
    static void becomeDiscoverableAndConnectable();
    static void discoverDevices(void(*cb)(DeviceList&));
};

#endif
