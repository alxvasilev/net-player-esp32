#include <esp_http_server.h>
#include <sys/socket.h>
#include "dlna.hpp"
#include <esp_log.h>
#include "utils.hpp"
#include "incfile.hpp"
#include <mxml.h>

static constexpr const char* TAG = "dlna";

EMBED_TEXTFILE("../../dlna/deviceDesc.xml", xmlMain);
extern const char xmlMain[];
extern const int xmlMain_size;
EMBED_TEXTFILE("../../dlna/avtrans.xml", xmlAvTrans);
extern const char xmlAvTrans[];
extern const int xmlAvTrans_size;
EMBED_TEXTFILE("../../dlna/connmgr.xml", xmlConnMgr);
extern const char xmlConnMgr[];
extern const int xmlConnMgr_size;
EMBED_TEXTFILE("../../dlna/rendctl.xml", xmlRendCtl);
extern const char xmlRendCtl[];
extern const int xmlRendCtl_size;

static const char kSchemaPrefix[] = "urn:schemas-upnp-org:service:";
static const char kAvTransport[] = "AVTransport:1";
static const char kConnMgr[] = "ConnectionManager:1";
static const char kRendCtl[] = "RenderingControl:1";
static const char kSsdpRootDev[] = "ssdp:rootdevice";
static const char kServerHeader[] = "FreeRTOS/x UPnP/1.0 NetPlayer/x";

class Endian
{
    static constexpr uint32_t mU32 = 0x01020304;
    static constexpr uint8_t mView = (const uint8_t&)mU32;
public:
    static constexpr bool little = (mView == 0x04);
    static constexpr bool big = (mView == 0x01);
    static_assert(little || big, "Cannot determine endianness!");
private:
    Endian() = delete;
};
constexpr uint32_t ip4Addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    if (Endian::little) {
        return a | (b << 8) | (c << 16) | (d << 24);
    } else {
        return (a << 24) | (b << 16) | (c << 8) | d;
    }
}

static constexpr uint32_t kSsdpMulticastAddr = ip4Addr(239,255,255,250);

DlnaHandler::DlnaHandler(httpd_handle_t httpServer, const char* hostPort, AudioPlayer& player)
: mHttpServer(httpServer), mPlayer(player)
{
    strncpy(mHttpHostPort, hostPort, sizeof(mHttpHostPort));
    memset(&mAddr, 0, sizeof(mAddr));
    mAddr.sin_family = AF_INET;
    mAddr.sin_port = htons(1900);
    uint8_t macBin[6];
    esp_read_mac(macBin, ESP_MAC_WIFI_STA);
    binToHex(macBin, sizeof(macBin), mUuid, 0);
    mUuid[12] = 0;
}
DlnaHandler::~DlnaHandler()
{
    if (mSsdpSocket) {
        close(mSsdpSocket);
        mSsdpSocket = -1;
    }
    unregisterHttpHandlers();
}
bool DlnaHandler::start()
{
    if (mSsdpSocket != -1) {
        ESP_LOGW(TAG, "Already started");
        return true;
    }
    mSsdpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (mSsdpSocket < 0) {
        ESP_LOGW(TAG, "Error %s creating SSDP UDP socket", strerror(errno));
        return false;
    }
    u_int yes = 1;
    if (setsockopt(mSsdpSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        ESP_LOGE(TAG, "setsockopt(SO_REUSEADDR) failed with error %s", strerror(errno));
        closeSocket();
        return false;
    }
    mAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(mSsdpSocket, (struct sockaddr*)&mAddr, sizeof(mAddr)) < 0) {
         ESP_LOGE(TAG, "bind failed with error %s", strerror(errno));
         closeSocket();
         return false;
    }
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = kSsdpMulticastAddr; // remote addr
    printf("verify ip4Addr: %s\n", inet_ntoa(mreq.imr_multiaddr.s_addr));
    mreq.imr_interface.s_addr = htonl(INADDR_ANY); // local addr
    if (setsockopt(mSsdpSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) < 0) {
        ESP_LOGI(TAG, "setsockopt(IP_ADD_MEMBERSHIP) failed with error %s", strerror(errno));
        closeSocket();
        return false;
    }
    if (!registerHttpHandlers()) {
        ESP_LOGW(TAG, "Could not register DLNA descriptor URL handler");
        closeSocket();
        return false;
    }
    auto ret = xTaskCreate([](void* arg) {
        static_cast<DlnaHandler*>(arg)->ssdpRxTaskFunc();
    }, "SSDP listener", 3000, this, 10, &mSsdpRxTask);
    if (ret != pdPASS) {
        ESP_LOGW(TAG, "Error creating UPnP listener task");
        unregisterHttpHandlers();
        closeSocket();
        return false;
    }
    return true;
}
void DlnaHandler::closeSocket()
{
    if (mSsdpSocket >= 0) {
        close(mSsdpSocket);
        mSsdpSocket = -1;
    }
}

esp_err_t DlnaHandler::httpDlnaDescGetHandler(httpd_req_t *req)
{
    auto url = req->uri;
    ESP_LOGI(TAG, "DLNA desc request: %s", url);
    if (strncmp(url, "/dlna/", 6)) {
        ESP_LOGW(TAG, "DLNA desc http handler received URL not starting with '/dlna/'");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    url += 6;
    const char* resp = nullptr;
    size_t respSize = 0;
    if (strcmp(url, "main.xml") == 0) {
        resp = xmlMain;
        respSize = xmlMain_size;
    }
    else if (strcmp(url, "avtrans.xml") == 0) {
        resp = xmlAvTrans;
        respSize = xmlAvTrans_size;
    }
    else if (strcmp(url, "connmgr.xml") == 0) {
        resp = xmlConnMgr;
        respSize = xmlConnMgr_size;
    }
    else if (strcmp(url, "rendctl.xml") == 0) {
        resp = xmlRendCtl;
        respSize = xmlRendCtl_size;
    }
    if (!resp) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/xml");
    httpd_resp_send(req, resp, respSize);
    return ESP_OK;
}

esp_err_t DlnaHandler::httpDlnaCommandHandler(httpd_req_t* req)
{
    // Handle control commands
    auto url = req->uri;
    ESP_LOGI(TAG, "DLNA command request: %s", url);
    if (strncmp(url, "/dlna/", 6)) {
        ESP_LOGW(TAG, "DLNA ctrl http handler received URL not starting with '/dlna/'");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    esp_err_t(*handler)(httpd_req_t*, const char*, mxml_node_t*);
    url += 6;
    if (strcmp(url, "AVTransport/ctrl") == 0) {
        handler = handleAvTransportCommand;
    } else if (strcmp(url, "ConnectionManager/ctrl")) {
        handler = handleConnMgrCommand;
    } else if (strcmp(url, "RenderingControl/ctrl") == 0) {
        handler = handleRenderCtlCommand;
    } else { // handle request for service description XMLs
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int32_t contentLen = req->content_len;
    if (!contentLen || contentLen > 2048) {
        ESP_LOGW(TAG, "Control request has too large or missing postdata: content-length: %d", contentLen);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large or missing POSTDATA");
        return ESP_FAIL;
    }
    std::unique_ptr<char, decltype(std::free)*> xml((char*)utils::mallocTrySpiram(contentLen + 1), std::free);
    auto recvLen = httpd_req_recv(req, xml.get(), contentLen);
    if (recvLen != contentLen) {
        ESP_LOGW(TAG, "Ctrl command: error receiving postdata: %s",
            recvLen < 0 ? esp_err_to_name(recvLen) : "received less than expected");
        return ESP_FAIL;
    }
    xml.get()[contentLen] = 0; // null-terminate postdata string
    printf("rx XML cmd: %s\n", xml.get());
    auto tree = mxmlLoadString(NULL, xml.get(), MXML_NO_CALLBACK);
    mxml_node_t* sBody = mxmlFindPath(tree, "s:Envelope/s:Body/");
    if (!sBody) {
        ESP_LOGW(TAG, "s:Body tag not found in XML command");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing s:Body");
        return ESP_FAIL;
    }
    auto cmdNode = mxmlGetFirstChild(sBody);
    if (!cmdNode) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing u:<command> node");
        return ESP_FAIL;
    }
    const char* cmd = mxmlGetElement(cmdNode);
    if (!cmd || strncasecmp(cmd, "u:", 2)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command XML node does not start with u:");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/xml");
    return handler(req, cmd+2, cmdNode);
}
esp_err_t DlnaHandler::handleAvTransportCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode)
{
    printf("handleAvTransportCommand: %s\n", cmd);
    return ESP_OK;
}
esp_err_t DlnaHandler::handleConnMgrCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode)
{
    printf("handleConnMgrCommand: %s\n", cmd);
    return ESP_OK;
}
esp_err_t DlnaHandler::handleRenderCtlCommand(httpd_req_t* req, const char* cmd, mxml_node_t* cmdNode)
{
    printf("handleRenderCtlCommand: %s\n", cmd);
    return ESP_OK;
}
bool DlnaHandler::registerHttpHandlers()
{
    httpd_uri_t desc = {
        .uri       = "/dlna/*",
        .method    = HTTP_GET,
        .handler   = httpDlnaDescGetHandler,
        .user_ctx  = this
    };
    if (httpd_register_uri_handler(mHttpServer, &desc) != ESP_OK) {
        goto fail;
    }
    desc.method = HTTP_POST;
    desc.handler = httpDlnaCommandHandler;
    if (httpd_register_uri_handler(mHttpServer, &desc) != ESP_OK) {
        goto fail;
    }
    return true;
fail:
    ESP_LOGE(TAG, "Error registering http handler(s)");
    unregisterHttpHandlers();
    return false;
}
void DlnaHandler::unregisterHttpHandlers()
{
    httpd_unregister_uri(mHttpServer, "/dlna");
}

void DlnaHandler::ssdpRxTaskFunc()
{
    SvcName svcName = {.prefix = nullptr, .name = "upnp:rootdevice"};
    sendNotifyBye();
    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyBye();

    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyAlive(svcName);
    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyAlive(svcName);

    while (!mTerminate) {
        socklen_t addrLen = sizeof(mAddr);
        int len = recvfrom(mSsdpSocket, mMsgBuf, sizeof(mMsgBuf), 0, (struct sockaddr*)&mAddr, &addrLen);
        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom error: %s", strerror(errno));
            break;
        }
        mMsgBuf[len] = '\0';
        bool supported = parseSsdpRequest(mMsgBuf, len + 1, svcName);
        if (!supported) {
            continue;
        }
        vTaskDelay(10);
        auto ip = mAddr.sin_addr.s_addr;
        if (svcName.name) {
            sendReply(svcName, ip);
        } else {
            // send packets for all services
            svcName.prefix = kSchemaPrefix;
            svcName.name = kAvTransport;
            sendReply(svcName, ip);
            svcName.name = kRendCtl;
            sendReply(svcName, ip);
            svcName.name = kConnMgr;
            sendReply(svcName, ip);
        }
    }
}

void DlnaHandler::sendReply(const SvcName& svcName, uint32_t ip)
{
    const char* svcPrefix = svcName.prefix ? svcName.prefix : "";
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
       "HTTP/1.1 200 OK\r\n"
       "Location: %s/dlna/main.xml\r\n"
       "Ext:\r\n"
       "USN: uuid:%s::%s%s\r\n"
       "Cache-Control: max-age=1800\r\n"
       "ST: %s%s\r\n"
       "Server: %s\r\n\r\n",
       mHttpHostPort, mUuid, svcPrefix, svcName.name, svcPrefix, svcName.name, kServerHeader
    );
    sendPacket(len, ip);
}
void DlnaHandler::sendNotifyBye()
{
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
        "NOTIFY * HTTP/1.1\r\n"
        "Host: 239.255.255.250:1900\r\n"
        "NTS: ssdp:byebye\r\n"
        "NT: uuid:%s\r\n"
        "USN: uuid:%s\r\n\r\n",
        mUuid, mUuid
    );
    sendPacket(len, kSsdpMulticastAddr);
}
void DlnaHandler::sendNotifyAlive(const SvcName& svcName)
{
    const char* svcPrefix = svcName.prefix ? svcName.prefix : "";
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
        "NOTIFY * HTTP/1.1\r\n"
        "Location: %s/dlna/main.xml\r\n"
        "Host: 239.255.255.250:1900\r\n"
        "Cache-Control: max-age=1800\r\n"
        "NTS: ssdp:alive\r\n"
        "NT: %s%s\r\n"
        "USN: uuid:%s::%s%s\r\n"
        "Server: %s\r\n\r\n",
        mHttpHostPort, svcPrefix, svcName.name, mUuid, svcPrefix, svcName.name, kServerHeader
    );
    sendPacket(len, kSsdpMulticastAddr);
}
void DlnaHandler::sendPacket(int len, uint32_t ip)
{
    printf("sending: %.*s\n", len, mMsgBuf);
    mAddr.sin_addr.s_addr = ip;
    int ret = sendto(mSsdpSocket, mMsgBuf, len, 0, (const sockaddr*)&mAddr, sizeof(mAddr));
    if (ret != len) {
        if (ret < 0) {
            ESP_LOGW(TAG, "sendto error: %d (%s)", errno, strerror(errno));
        } else {
            ESP_LOGW(TAG, "sendto() sent less than expected");
        }
    }
}

bool DlnaHandler::parseSsdpRequest(char* str, int len, SvcName& svcName)
{
    svcName.prefix = svcName.name = nullptr;
    char msearch[] = "M-SEARCH ";
    if (strncasecmp(str, msearch, sizeof(msearch) - 1)) {
        return false;
    }
    //printf("ssdp M-SEARCH req:\n%s\n", str);
    char* end = str + sizeof(msearch) - 1;
    for (; *end; end++) {
        if (*end == '\n') {
            break;
        }
    }
    if (*end == 0) {
        ESP_LOGW(TAG, "SSDP request contains no newlines");
        return false;
    }
    end++;

    KeyValParser params(end, len - (end - str));
    params.parse('\n', ':', KeyValParser::kTrimSpaces | KeyValParser::kKeysToLower);
    for (auto& kv: params.keyVals()) {
        auto& val = kv.val;
        if (val.str[val.len-1] == '\r') {
            val.str[--val.len] = 0;
        }
    }
    auto st = params.strVal("st").str;
    if (!st) {
        return false;
    } else if (strcasecmp(st, "ssdp:all") == 0) {
        return true;
    } else if (strcasecmp(st, kSsdpRootDev) == 0) {
        svcName.name = kSsdpRootDev;
        return true;
    } else if (strncasecmp(st, kSchemaPrefix, sizeof(kSchemaPrefix) - 1) == 0) {
        st += sizeof(kSchemaPrefix) - 1;
        svcName.prefix = kSchemaPrefix;
        if (strncasecmp(st, kAvTransport, sizeof(kAvTransport)-2) == 0) { // skip the :version part
            svcName.name = kAvTransport;
            return true;
        }
        else if (strncasecmp(st, kConnMgr, sizeof(kConnMgr) - 2) == 0) {
           svcName.name = kConnMgr;
           return true;
        }
        else if (strncasecmp(st, kRendCtl, sizeof (kRendCtl) - 2) == 0) {
            svcName.name = kRendCtl;
            return true;
        } else {
            return false;
        }
    }
    return false; // should not be reached
}
