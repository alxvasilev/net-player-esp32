#ifndef DLNA_HPP_HEADER_
#define DLNA_HPP_HEADER_
#include <string>
class AudioPlayer;
typedef struct _mxml_node_s mxml_node_t;

class DlnaHandler {
protected:
    struct SvcName {
        const char* prefix;
        const char* name;
    };
    static constexpr const char* kSsdpMulticastGroup = "239.255.255.250";
    enum { kSsdpPort = 1900 };
    int mSsdpSocket = -1;
    httpd_handle_t mHttpServer;
    char mHttpHostPort[48];
    sockaddr_in mAddr;
    TaskHandle_t mSsdpRxTask;
    AudioPlayer& mPlayer;
    std::string mAvTransportUri;
    char mUuid[13];
    bool mTerminate = false;
    char mMsgBuf[480];
    void sendPacket(int len, uint32_t ip);
    void sendReply(const SvcName& svcName, uint32_t ip);
    void sendNotifyAlive(const SvcName& svcName);
    void sendNotifyBye();
    void ssdpRxTaskFunc();
    bool registerHttpHandlers();
    void unregisterHttpHandlers();
    void closeSocket();
    bool parseSsdpRequest(char* str, int len, SvcName& svcName);
    static esp_err_t httpDlnaDescGetHandler(httpd_req_t* req);
    static esp_err_t httpDlnaCommandHandler(httpd_req_t* req);
    bool handleAvTransportCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode, std::string& result);
    bool handleConnMgrCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode, std::string& result);
    bool handleRenderCtlCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode, std::string& result);

public:
    DlnaHandler(httpd_handle_t httpServer, const char* hostPort, AudioPlayer& player);
    ~DlnaHandler();
    bool start();
};
#endif
