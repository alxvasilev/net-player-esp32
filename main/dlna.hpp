#ifndef DLNA_HPP_HEADER_
#define DLNA_HPP_HEADER_
#include <string>
#include <memory>
#include "utils.hpp"

class AudioPlayer;
struct TrackInfo;
namespace tinyxml2 {
    class XMLElement;
};
class DlnaHandler {
protected:
    static constexpr const char* kSsdpMulticastGroup = "239.255.255.250";
    enum { kSsdpPort = 1900 };
    int mSsdpSocket = -1;
    httpd_handle_t mHttpServer;
    char mHttpHostPort[48];
    sockaddr_in mAddr;
    TaskHandle_t mSsdpRxTask;
    AudioPlayer& mPlayer;
    unique_ptr_mfree<TrackInfo> mQueuedTrack;
    char mUuid[13];
    bool mTerminate = false;
    char mMsgBuf[480];
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
    bool handleAvTransportCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);
    bool handleConnMgrCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);
    bool handleRenderCtlCommand(httpd_req_t* req, const char* cmd, const tinyxml2::XMLElement& cmdNode, std::string& result);

public:
    DlnaHandler(httpd_handle_t httpServer, const char* hostPort, AudioPlayer& player);
    ~DlnaHandler();
    bool start();
};
#endif
