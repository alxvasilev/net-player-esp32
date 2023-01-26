#ifndef DLNA_HPP_HEADER_
#define DLNA_HPP_HEADER_
#include <string>
#include <memory>
#include <list>
#include "utils.hpp"
#include <trackDelete.hpp>

class AudioPlayer;
struct TrackInfo;
namespace tinyxml2 {
    class XMLElement;
}
namespace http {
    class Server;
}
class DlnaHandler {
protected:
    struct EventSubscription
    {
        enum: uint8_t { kServiceMediaRenderer = 1 };
    protected:
        static constexpr const char* kSidPrefix = "uuid:564dc06e-4609-4f08-9430-7074"; //"uuid:00000000-0000-0000-0000-0000";
        uint32_t mSeqNo = 0;
    public:
        static uint32_t sSidCounter;
        uint32_t sid;
        uint32_t tsTill;
        DynBuffer callbackUrl;
        uint8_t service;
        static uint32_t parseSid(char* sid, int len=-1);
        EventSubscription(uint8_t aService): sid(++sSidCounter), service(aService) {}
        const char* strSid(char* buf, size_t bufLen) const;
        bool notify(const char* xml);
        bool notifySubscribed();
        ~EventSubscription();
    };
    static constexpr const char* kSsdpMulticastGroup = "239.255.255.250";
    enum { kSsdpPort = 1900 };
    // these are accessed  by the SSDP thread
    int mSsdpSocket = -1;
    char mMsgBuf[480];
    sockaddr_in mAddr;
    //====
    http::Server& mHttpServer;
    char mHttpHostPort[48];
    TaskHandle_t mSsdpRxTask;
    AudioPlayer& mPlayer;
    unique_ptr_mfree<TrackInfo> mQueuedTrack;
    char mUuid[13];
    bool mTerminate = false;
    std::list<EventSubscription> mEventSubs;
    void sendPacket(int len, uint32_t ip);
    void sendReply(const char* name, uint32_t ip, const char* prefix);
    void sendUuidReply(uint32_t ip);
    void sendNotifyAlive(const char* svcName, const char* prefix);
    void sendNotifyBye();
    void ssdpRxTaskFunc();
    bool registerHttpHandlers();
    void unregisterHttpHandlers();
    void closeSocket();
    void handleSsdpRequest(char* str, int len, uint32_t ip);
    static esp_err_t httpDlnaDescGetHandler(httpd_req_t* req);
    static esp_err_t httpDlnaCommandHandler(httpd_req_t* req);
    static esp_err_t httpDlnaSubscribeHandler(httpd_req_t* req);
    static esp_err_t httpDlnaUnsubscribeHandler(httpd_req_t* req);
    bool handleAvTransportCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);
    bool handleConnMgrCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);
    bool handleRenderCtlCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);
    EventSubscription* eventSubscriptionBySid(uint32_t sid);
    void notifyEvent(const char* xml);
public:
    DlnaHandler(http::Server& httpServer, const char* hostPort, AudioPlayer& player);
    ~DlnaHandler();
    bool start();
    void notifyVolumeChange(int vol);
    void notifyMute(bool mute, int vol);
};
#endif
