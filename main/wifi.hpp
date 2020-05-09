#ifndef WIFI_HPP
#define WIFI_HPP
#include "eventGroup.hpp"

class WifiClient
{
public:
    typedef void(*ConnGaveUpHandler)();
protected:
    enum { kBitConnected = 1, kBitAborted = 2 };
    enum { kMaxConnectRetries = 10 };
    EventGroup mEvents;
    int mRetryNum = 0;
    ConnGaveUpHandler mConnRetryGaveUpHandler;
    static esp_err_t eventHandler(void *ctx, system_event_t *event);
public:
    ~WifiClient();
    bool start(const char* ssid, const char* key, ConnGaveUpHandler onGaveUp=nullptr);
    bool waitForConnect(int msTimeout);
    void stop();
};

#endif
