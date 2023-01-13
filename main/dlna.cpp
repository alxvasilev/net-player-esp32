#include <esp_http_server.h>
#include <sys/socket.h>
#include "dlna.hpp"
#include <esp_log.h>
#include "utils.hpp"
#include "incfile.hpp"
#include <tinyxml2.h>
#include "dlna-parse.hpp"
#include "audioPlayer.hpp"
#include <sstream>

static constexpr const char* TAG = "dlna";

EMBED_TEXTFILE("../../dlna/deviceDesc.xml", xmlMain);
extern const char xmlMain[];
extern const int xmlMain_size;
EMBED_TEXTFILE("../../dlna/avtrans.xml", xmlAvTrans);
extern const char xmlAvTrans[];
extern const int xmlAvTrans_size;
EMBED_TEXTFILE("../../dlna/rendctl.xml", xmlRendCtl);
extern const char xmlRendCtl[];
extern const int xmlRendCtl_size;
EMBED_TEXTFILE("../../dlna/connmgr.xml", xmlConnMgr);
extern const char xmlConnMgr[];
extern const int xmlConnMgr_size;

const char kUpnpServiceSchema[] = "urn:schemas-upnp-org:service:";
const char kUpnpDeviceSchema[] = "urn:schemas-upnp-org:device:";
static const char kMediaRenderer[] = "MediaRenderer:1";
static const char kAvTransport[] = "AVTransport:1";
static const char kConnMgr[] = "ConnectionManager:1";
static const char kRendCtl[] = "RenderingControl:1";
static const char kSsdpRootDev[] = "ssdp:rootdevice";
static const char kUpnpRootdevice[] = "upnp:rootdevice";
static const char kServerHeader[] = "FreeRTOS/x UPnP/1.0 NetPlayer/x";
#define kUuidPrefix "12345678-abcd-ef12-cafe-"

static constexpr uint32_t kSsdpMulticastAddr = utils::ip4Addr(239,255,255,250);

using namespace tinyxml2;

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
    else if (strcmp(url, "rendctl.xml") == 0) {
        resp = xmlRendCtl;
        respSize = xmlRendCtl_size;
    }
    else if (strcmp(url, "connmgr.xml") == 0) {
        resp = xmlConnMgr;
        respSize = xmlConnMgr_size;
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
    //ESP_LOGI(TAG, "\e[95mDLNA command request: %s", url);
    if (strncmp(url, "/dlna/", 6)) {
        ESP_LOGW(TAG, "DLNA ctrl http handler received URL not starting with '/dlna/'");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    bool(DlnaHandler::*handler)(httpd_req_t*, const char*, const XMLElement&, std::string&);
    url += 6;
    if (strcmp(url, "AVTransport/ctrl") == 0) {
        handler = &DlnaHandler::handleAvTransportCommand;
    } else if (strcmp(url, "RenderingControl/ctrl") == 0) {
        handler = &DlnaHandler::handleRenderCtlCommand;
    } else if (strcmp(url, "ConnectionManager/ctrl") == 0) {
        handler = &DlnaHandler::handleConnMgrCommand;
    }else { // handle request for service description XMLs
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int32_t contentLen = req->content_len;
    if (!contentLen || contentLen > 3000) {
        ESP_LOGW(TAG, "Control request has too large or missing postdata: content-length: %d", contentLen);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Too large or missing POSTDATA");
        return ESP_FAIL;
    }
    unique_ptr_mfree<char> strXml((char*)utils::mallocTrySpiram(contentLen + 1));
    auto recvLen = httpd_req_recv(req, strXml.get(), contentLen);
    if (recvLen != contentLen) {
        ESP_LOGW(TAG, "Ctrl command: error receiving postdata: %s",
            recvLen < 0 ? esp_err_to_name(recvLen) : "received less than expected");
        if (recvLen > 0) {
            strXml.get()[recvLen] = 0; // just in case
            ESP_LOGW(TAG, "Start of incomplete postdata: '%.*s'", (std::min(recvLen, 128)), strXml.get());
        }
        return ESP_FAIL;
    }
    strXml.get()[contentLen] = 0; // null-terminate postdata string
    //printf("rx XML cmd: %s\n", xml.get());
    tinyxml2::XMLDocument xml;
    auto err = xml.Parse(strXml.get(), contentLen);
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Error parsing XML");
        return ESP_FAIL;
    }
    strXml.reset();
    auto node = xmlFindPath(&xml, "s:Envelope/s:Body");
    if (!node) {
        ESP_LOGW(TAG, "s:Body tag not found in XML command");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing s:Body");
        return ESP_FAIL;
    }
    node = node->FirstChildElement();
    if (!node) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing u:<command> node");
        return ESP_FAIL;
    }
    const char* cmd = node->Name();
    if (!cmd || strncasecmp(cmd, "u:", 2)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command XML node does not start with u:");
        return ESP_FAIL;
    }
    const char* cmdXmlns = node->Attribute("xmlns:u");
    if (!cmdXmlns) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command XML node does not have xmlns:u attr");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Received command '\e[95m%s\e[0m' (for service %s)\n", cmd, url);

    auto self = static_cast<DlnaHandler*>(req->user_ctx);
    std::string result =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><";
    result.append(cmd).append("Response xmlns:u=\"")
          .append(cmdXmlns).append("\">");
    bool ok = (self->*handler)(req, cmd+2, *node, result);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Error parsing command");
        return ESP_FAIL;
    }
    result.append("</").append(cmd).append("Response></s:Body></s:Envelope>");
    httpd_resp_set_type(req, "text/xml");
    printf("DLNA tx(%zu):\n%s\n", result.size(), result.c_str());
    httpd_resp_send(req, result.c_str(), result.size());
    return ESP_OK;
}
const char* kHmsZeroTime = "0:00:00.000";
bool DlnaHandler::handleAvTransportCommand(httpd_req_t* req, const char* cmd, const XMLElement& cmdNode, std::string& result)
{
    MutexLocker locker(mPlayer.mutex);
    if (strcasecmp(cmd, "Stop") == 0 || (strcasecmp(cmd, "Pause") == 0)) {
        mPlayer.pause();
        return true;
    }
    else if (strcasecmp(cmd, "GetTransportInfo") == 0) {
        result.reserve(256);
        result.append("<CurrentTransportState>").append(
               (mPlayer.mode() == AudioPlayer::kModeDlna) && mPlayer.isPlaying() ? "PLAYING" : "STOPPED")
              .append(
                  "</CurrentTransportState><CurrentTransportStatus>OK</CurrentTransportStatus>"
                  "<CurrentSpeed>1</CurrentSpeed>");
        return true;
    }
    else if (strcasecmp(cmd, "SetAVTransportURI") == 0) {
        mQueuedTrack.reset();
        const char* url = xmlGetChildText(cmdNode, "CurrentURI");
        if (!url) {
            return false;
        }
        do {
            auto strInfo = xmlGetChildText(cmdNode, "CurrentURIMetaData");
            if (!strInfo) {
                break;
            }
            XMLDocument info;
            if (info.Parse(strInfo)) {
                ESP_LOGW(TAG, "Error parsing DIDL-Lite metadata");
                break;
            }
            auto item = xmlFindPath(&info, "DIDL-Lite/item");
            if (!item) {
                break;
            }
            auto trkInfo = TrackInfo::Create(url, xmlGetChildText(*item, "dc:title"),
                xmlGetChildText(*item, "upnp:artist"), parseHmsTime(xmlGetChildAttr(*item, "res", "duration")));
            mQueuedTrack.reset(trkInfo);
            return true;
        } while(false);
        mQueuedTrack.reset(TrackInfo::Create(url, nullptr, nullptr, 0));
        return true;
    }
    else if (strcasecmp(cmd, "Play") == 0) {
        if (!mQueuedTrack) {
            return false;
        }
        mPlayer.playUrl(mQueuedTrack.release(), AudioPlayer::kModeDlna); //TODO: Implement track info
        return true;
    }
    else if (strcasecmp(cmd, "GetPositionInfo") == 0) {
        result.append("<Track>1</Track><TrackMetaData></TrackMetaData><TrackDuration>");
        auto trkInfo = mPlayer.trackInfo();
        if (trkInfo) {
            auto posHms = msToHmsString(mPlayer.positionTenthSec() * 100);
            result.append(msToHmsString(trkInfo->durationMs))
                  .append("</TrackDuration><TrackURI>").append(trkInfo->url)
                  .append("</TrackURI><RelTime>").append(posHms)
                  .append("</RelTime><AbsTime>").append(posHms);
        } else {
            result.append(kHmsZeroTime).append("</TrackDuration><TrackURI></TrackURI><RelTime>")
                  .append(kHmsZeroTime).append("</RelTime><AbsTime>").append(kHmsZeroTime);
        }
        result.append("</AbsTime><RelCount>0</RelCount><AbsCount>0</AbsCount>");
        return true;
    } else {
        return false;
    }
}
bool DlnaHandler::handleConnMgrCommand(httpd_req_t* req, const char* cmd, const XMLElement& cmdNode, std::string& result)
{
    if (strcasecmp(cmd, "GetProtocolInfo") == 0) {
        result.reserve(512);
        result.append("<Source></Source><Sink>"
            "http-get:*:audio/flac:*,http-get:*:audio/x-flac:*,"
            "http-get:*:audio/mp3:*,http-get:*:audio/mpeg:*,"
            "http-get:*:audio/aac:*,http-get:*:audio/x-aac:*,http-get:*:audio/aacp:*,"
            "http-get:*:audio/ogg:*,http-get:*:application/ogg:*"
            "</Sink>");
        return true;
    } else {
        return false;
    }
}
bool DlnaHandler::handleRenderCtlCommand(httpd_req_t* req, const char* cmd, const XMLElement& cmdNode, std::string& result)
{
    MutexLocker locker(mPlayer.mutex);
    if (strcasecmp(cmd, "SetVolume") == 0) {
        const char* strVol = xmlGetChildText(cmdNode, "DesiredVolume");
        if (!strVol) {
            return false;
        }
        int vol = parseInt(strVol, -1);
        if (vol < 0) {
            ESP_LOGW(TAG, "%s: Invalid volume level integer '%s'", cmd, strVol);
            return false;
        }
        mPlayer.volumeSet(vol);
        return true;
    }
    else if (strcasecmp(cmd, "SetMute") == 0) {
        auto strMute = xmlGetChildText(cmdNode, "DesiredMute");
        if (!strMute) {
            return false;
        }
        auto mute = parseInt(strMute, -1);
        if (mute < 0) {
            ESP_LOGW(TAG, "%s: Invalid mute value '%s'", cmd, strMute);
            return false;
        }
        mute ? mPlayer.mute() : mPlayer.unmute();
        return true;
    }
    return false;
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
    static constexpr const char* kRootDev = "upnp:rootdevice";
    sendNotifyBye();
    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyBye();

    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyAlive(kRootDev, kUpnpDeviceSchema);
    vTaskDelay(portTICK_PERIOD_MS / 10);
    sendNotifyAlive(kRootDev, kUpnpDeviceSchema);

    while (!mTerminate) {
        socklen_t addrLen = sizeof(mAddr);
        int len = recvfrom(mSsdpSocket, mMsgBuf, sizeof(mMsgBuf), 0, (struct sockaddr*)&mAddr, &addrLen);
        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom error: %s", strerror(errno));
            break;
        }
        mMsgBuf[len] = '\0';
        vTaskDelay(10);
        handleSsdpRequest(mMsgBuf, len + 1, mAddr.sin_addr.s_addr);
    }
}

void DlnaHandler::sendReply(const char* name, uint32_t ip, const char* prefix)
{
    if (!prefix) {
        prefix = "";
    }
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
       "HTTP/1.1 200 OK\r\n"
       "Location: %s/dlna/main.xml\r\n"
       "Ext:\r\n"
       "USN: uuid:" kUuidPrefix "%s::%s%s\r\n"
       "ST: %s%s\r\n"
       "Cache-Control: max-age=1800\r\n"
       "Server: %s\r\n\r\n",
       mHttpHostPort, mUuid, prefix, name, prefix, name, kServerHeader
    );
    sendPacket(len, ip);
}
void DlnaHandler::sendUuidReply(uint32_t ip)
{
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
       "HTTP/1.1 200 OK\r\n"
       "Location: %s/dlna/main.xml\r\n"
       "Ext:\r\n"
       "USN: uuid:" kUuidPrefix "%s\r\n"
       "ST: uuid:" kUuidPrefix "%s\r\n"
       "Cache-Control: max-age=1800\r\n"
       "Server: %s\r\n\r\n",
       mHttpHostPort, mUuid, mUuid, kServerHeader
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
void DlnaHandler::sendNotifyAlive(const char* svcName, const char* prefix)
{
    if (!prefix) {
        prefix = "";
    }
    int len = snprintf(mMsgBuf, sizeof(mMsgBuf),
        "NOTIFY * HTTP/1.1\r\n"
        "Location: %s/dlna/main.xml\r\n"
        "Host: 239.255.255.250:1900\r\n"
        "Cache-Control: max-age=1800\r\n"
        "NTS: ssdp:alive\r\n"
        "NT: %s%s\r\n"
        "USN: uuid:%s::%s%s\r\n"
        "Server: %s\r\n\r\n",
        mHttpHostPort, prefix, svcName, mUuid, prefix, svcName, kServerHeader
    );
    sendPacket(len, kSsdpMulticastAddr);
}
void DlnaHandler::sendPacket(int len, uint32_t ip)
{
    printf("sending: %.*s to %s\n", len, mMsgBuf, inet_ntoa(ip));
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

void DlnaHandler::handleSsdpRequest(char* str, int len, uint32_t ip)
{
    char msearch[] = "M-SEARCH ";
    if (strncasecmp(str, msearch, sizeof(msearch) - 1)) {
        return;
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
        return;
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
        return;
    }
    else if (strcasecmp(st, "ssdp:all") == 0) {
        sendReply(kUpnpRootdevice, ip, nullptr);
        sendUuidReply(ip);
        sendReply(kMediaRenderer, ip, kUpnpDeviceSchema);
        sendReply(kAvTransport, ip, kUpnpServiceSchema);
        sendReply(kRendCtl, ip, kUpnpServiceSchema);
    }
    else if (strcasecmp(st, kSsdpRootDev) == 0) {
        sendReply(kSsdpRootDev, ip, nullptr);
    }
    else if (strncasecmp(st, kUpnpServiceSchema, sizeof(kUpnpServiceSchema) - 1) == 0) {
        st += sizeof(kUpnpServiceSchema) - 1;
        if (strncasecmp(st, kAvTransport, sizeof(kAvTransport) - 2) == 0) { // skip the :version part
            sendReply(kAvTransport, ip, kUpnpServiceSchema);
        }
        else if (strncasecmp(st, kMediaRenderer, sizeof(kMediaRenderer) - 2) == 0) {
            sendReply(kMediaRenderer, ip, kUpnpServiceSchema);
        }
        else if (strncasecmp(st, kRendCtl, sizeof (kRendCtl) - 2) == 0) {
            sendReply(kRendCtl, ip, kUpnpServiceSchema);
        }
        else if (strncasecmp(st, kConnMgr, sizeof(kConnMgr) - 2) == 0) {
            sendReply(kConnMgr, ip, kUpnpServiceSchema);
        }
    }
}
