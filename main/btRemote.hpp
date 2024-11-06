#ifndef __BT_REMOTE_HPP
#define __BT_REMOTE_HPP

#include "esp_hidh.h"
#include "esp_hid_common.h"
#include <vector>

class BtRemote
{
protected:
    esp_hidh_dev_t* mHidDevice = nullptr;
    int8_t battLevel = 0;
    std::vector<std::vector<uint8_t>> btnReports;
    static void sHidhCallback(void * handler_args, esp_event_base_t base, int32_t id, void * event_data);
  public:
    BtRemote() {}
    bool init();
    bool openBtHidDevice(const uint8_t* addr, int bleAddrType=-1);
};
#endif
