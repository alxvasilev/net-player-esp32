#ifndef WIFI_HPP
#define WIFI_HPP
#include "eventGroup.hpp"

class WifiBase
{
protected:
    EventGroup mEvents;
    const bool mIsAp;
    void eventLoopCreateAndRegisterHandler(esp_event_handler_t handler);
public:
    enum { kBitConnected = 1, kBitAborted = 2 };
    WifiBase(bool isAp): mEvents(kBitAborted), mIsAp(isAp){}
    virtual ~WifiBase() {}
    bool isAp() const { return mIsAp; }
    bool waitForConnect(int msTimeout);
};

class WifiClient: public WifiBase
{
public:
    typedef void(*ConnGaveUpHandler)();
protected:
    enum { kMaxConnectRetries = 10 };
    int mRetryNum = 0;
    ConnGaveUpHandler mConnRetryGaveUpHandler;
    static void wifiEventHandler(void* userp, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data);
    static void gotIpEventHandler(void* userp, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data);
public:
    WifiClient(): WifiBase(false) {}
    ~WifiClient() override;
    bool start(const char* ssid, const char* key, ConnGaveUpHandler onGaveUp=nullptr);
    void stop();
};

class WifiAp: public WifiBase
{
protected:
    static void apEventHandler(void* userp, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data);
    void reconfigDhcpServer();
public:
    WifiAp(): WifiBase(true) {}
    ~WifiAp() override;
    void start(const char* ssid, const char* key, uint8_t chan);
    void stop();
};

#endif
