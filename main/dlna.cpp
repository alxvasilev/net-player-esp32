#include <esp_http_server.h>
#include <sys/socket.h>
#include "dlna.hpp"
#include <httpServer.hpp>
#include <esp_log.h>
#include "utils.hpp"
#include "incfile.hpp"
#include <tinyxml2.h>
#include "dlna-parse.hpp"
#include "audioPlayer.hpp"
//#include <sstream>
#include <functional>

static const char* TAG = "dlna";
static const char* TAG_NOTIFY = "dlna-notify";

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
uint32_t DlnaHandler::EventSubscription::sSidCounter = 0;

using namespace tinyxml2;

DlnaHandler::DlnaHandler(http::Server& httpServer, const char* hostPort, AudioPlayer& player)
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
    } else { // handle request for service description XMLs
        ESP_LOGW(TAG, "Request for unknown URL '%s'", url);
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
            ESP_LOGW(TAG, "Incomplete postdata: '%s'", strXml.get());
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
    //printf("DLNA tx(%zu):\n%s\n", result.size(), result.c_str());
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
            "http-get:*:audio/ogg:*,http-get:*:application/ogg:*,"
            "http-get:*:audio/wav:*,http-get:*:audio/wave:*,http-get:*:audio/x-wav:*,"
            "http-get:*:audio/L8:*,http-get:*:audio/L16:*,http-get:*:audio/L24:*,http-get:*:audio/L32:*"
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

bool httpGetHeader(httpd_req_t* req, const char* hdrName, const std::function<char*(size_t)>& cb)
{
    char errMsg[64];
    auto len = httpd_req_get_hdr_value_len(req, hdrName);
    if (!len) {
        snprintf(errMsg, sizeof(errMsg)-1, "No %s header present", hdrName);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errMsg);
        return false;
    }
    len++;
    char* buf = cb(len);
    if (!buf) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, hdrName, buf, len) != ESP_OK) {
        snprintf(errMsg, sizeof(errMsg)-1, "Could not copy %s header", hdrName);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errMsg);
        return false;
    }
    buf[len-1] = 0;
//  printf("header '%s' -> '%s'\n", hdrName, buf);
    return true;
}
const char* DlnaHandler::EventSubscription::strSid(char* buf, size_t bufLen) const
{
//    return "uuid:00000000-0000-4000-8000-000000000001";
    vtsnprintf(buf, bufLen, kSidPrefix, fmtHex32(sid));
    return buf;
}
uint32_t DlnaHandler::EventSubscription::parseSid(char* sid, int len)
{
    if (len < 0) {
        len = strlen(sid);
    } else {
        assert(sid[len] == 0);
    }

    if (len < 8) {
        return 0; // invalid
    }
    return parseInt(sid + len - 8, 0, 16);
}
bool DlnaHandler::EventSubscription::notifySubscribed(AudioPlayer& player)
{
    std::string xml;
    {
    MutexLocker locker(player.mutex);
    if (service == kEventsRendCtl) {
        xml.reserve(200);
        xml = "&lt;PresetNameList val=&quot;FactoryDefaults&quot; channel=&quot;Master&quot;/&gt;&lt;Mute val=&quot;";
        xml += player.isMuted() ? '1' : '0';
        xml += "&quot; channel=&quot;Master&quot;/&gt;&lt;Volume val=&quot;";
        appendAny(xml, player.volumeGet());
        xml += "&quot; channel=&quot;Master&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;";
    }
    else if (service == kEventsAVTransport) {
        xml.reserve(300);
        xml = "&lt;TransportPlaySpeed val=&quot;1&quot;/&gt;"
              "&lt;CurrentPlayMode val=&quot;NORMAL&quot;/&gt;&lt;TransportState val=&quot;";
        const TrackInfo* trkInfo;
        if (player.isPlaying()) {
            trkInfo = player.trackInfo();
            xml += "PLAYING";
        } else {
            trkInfo = nullptr;
            xml += "STOPPED";
        }
        xml += "&quot;/&gt;&lt;AVTransportURI val=&quot;";
        xml += trkInfo ? trkInfo->url : "";
        xml += "&quot;/&gt;&lt;CurrentTrackDuration val=&quot;";
        xml += trkInfo ? msToHmsString(trkInfo->durationMs) : kHmsZeroTime;
        xml += "&quot;/&gt;&lt;CurrentTransportActions val=&quot;Play&quot;/&gt;";
        xml += "&lt;CurrentTrack val=&quot;1&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;";
    }
    else {
        ESP_LOGW(TAG_NOTIFY, "notifySubscribed: Unsupported service %d", service);
        return false;
    }
    }
    auto fullXml = createEventXml(service, xml);
    printf("NOTIFY subscribed: %s\n", fullXml.c_str());
    return notify(fullXml);
}

bool DlnaHandler::EventSubscription::notify(const std::string& xml)
{
    esp_http_client_config_t cfg = {};
    cfg.url = callbackUrl.buf();
    cfg.timeout_ms = 1000;
    cfg.buffer_size = 32;
    cfg.method = HTTP_METHOD_NOTIFY;
    auto client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG_NOTIFY, "Could not create http client");
        return false;
    }
    esp_http_client_set_header(client, "NT", "upnp:event");
    esp_http_client_set_header(client, "NTS", "upnp:propchange");
    char buf[64];
    esp_http_client_set_header(client, "SID", strSid(buf, sizeof(buf)));
    toString(buf, sizeof(buf), ++mSeqNo);
    esp_http_client_set_header(client, "SEQ", buf);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");

    bool success = false;
    do {
        esp_err_t err;
        if ((err = esp_http_client_open(client, xml.size())) != ESP_OK) {
            ESP_LOGW(TAG_NOTIFY, "Error connecting to target: %s", esp_err_to_name(err));
            break;
        }
        if (esp_http_client_write(client, xml.c_str(), xml.size()) < 0) {
            ESP_LOGW(TAG_NOTIFY, "Error posting xml: %s", strerror(errno));
            break;
        }
        auto responseLen = esp_http_client_fetch_headers(client);
        if (responseLen < 0) {
            ESP_LOGW(TAG_NOTIFY, "NOTIFY request '%s': Got negative content-length %d in response, status is %d", cfg.url,
                     responseLen, esp_http_client_get_status_code(client));
            break;
        }
        auto code = esp_http_client_get_status_code(client);
        if (code != 200) {
            ESP_LOGW(TAG_NOTIFY, "NOTIFY request '%s' failed with code %d, response len: %d",
                     cfg.url, code, responseLen);
            break;
        }
        if (responseLen > 0) {
            char buf[32];
            int nread;
            while((nread = esp_http_client_read(client, buf, sizeof(buf))) > 0);
            if (nread < 0) {
                ESP_LOGW(TAG_NOTIFY, "Error reading response body: %s", esp_err_to_name(err));
            }
        }
        success = true;
    } while(0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}
DlnaHandler::EventSubscription::~EventSubscription()
{
    ESP_LOGI(TAG_NOTIFY, "Deleting event subscription %u", sid);
}
void DlnaHandler::notify(EventSrc service, std::string& xml)
{
    mHttpServer.queueWork([this, service, xml = std::move(xml)]() {
        this->doNotify(service, xml);
    });
}
void DlnaHandler::doNotify(EventSrc service, const std::string& innerXml)
{
    auto xml = createEventXml(service, innerXml);
    printf("NOTIFY tx: %s\n", xml.c_str());
    uint32_t now = esp_timer_get_time() / 1000000;
    for (auto it = mEventSubs.begin(); it != mEventSubs.end();) {
        if (it->tsTill < now) {
            it = mEventSubs.erase(it);
            continue;
        }
        if (it->service == service) {
            it->notify(xml);
        }
        it++;
    }
}
const char* DlnaHandler::eventXmlFromType(EventSrc service)
{
    if (service == kEventsRendCtl) {
        return "RCS";
    } else if (service == kEventsAVTransport) {
        return "AVT";
    } else {
        assert(false);
        return nullptr;
    }
}
std::string DlnaHandler::createEventXml(EventSrc service, const std::string& inner)
{
    std::string result;
    result.reserve(inner.size() + 260);
    result = "<?xml version=\"1.0\"?><e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><LastChange>"
             "&lt;Event xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/";
    result.append(eventXmlFromType(service)).append("/&quot;&gt;&lt;InstanceID val=&quot;0&quot;&gt;");
    result.append(inner);
    result.append("</LastChange></e:property></e:propertyset>");
    return result;
}

void DlnaHandler::notifyVolumeChange(int vol)
{
    std::string xml;
    xml.reserve(100);
    xml = "&lt;Volume val=&quot;";
    appendAny(xml, vol);
    xml.append("&quot; channel=&quot;Master&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;");
    notify(kEventsRendCtl, xml);
}

void DlnaHandler::notifyMute(bool mute, int vol)
{
    std::string xml;
    xml.reserve(128);
    xml = "&lt;Mute val=&quot;";
    xml += (mute ? '1' : '0');
    xml.append("&quot; channel=&quot;Master&quot;/&gt;");
    if (!mute && vol > -1) {
        xml.append("&lt;Volume val=&quot;");
        appendAny(xml, vol);
        xml.append("&quot; channel=&quot;Master&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;");
    }
    notify(kEventsRendCtl, xml);
}
void DlnaHandler::notifyPlayStart()
{
    std::string xml;
    xml.reserve(200);
    std::string dur;
    auto trkInfo = mPlayer.trackInfo();
    dur = trkInfo ? msToHmsString(trkInfo->durationMs) : kZeroHmsTime;
    xml.append("&lt;TransportState val=&quot;PLAYING&quot;/&gt;&lt;CurrentTrackURI val=&quot;")
       .append(mPlayer.url())
       .append("&quot;/&gt;&lt;CurrentMediaDuration val=&quot;").append(dur)
       .append("&quot;/&gt;&lt;CurrentTrackDuration val=&quot;").append(dur)
       .append("&quot;/&gt;&lt;CurrentTransportActions val=&quot;Stop&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;");
    notify(kEventsAVTransport, xml);
}
void DlnaHandler::notifyPlayStop()
{
    std::string xml = "&lt;TransportState val=&quot;STOPPED&quot;/&gt;&lt;CurrentTransportActions val=&quot;Play&quot;/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;";
    notify(kEventsAVTransport, xml);
}
DlnaHandler::EventSubscription* DlnaHandler::eventSubscriptionBySid(uint32_t sid)
{
    for (auto it = mEventSubs.begin(); it != mEventSubs.end(); it++) {
        if (it->sid == sid) {
            return &(*it);
        }
    }
    return nullptr;
}
esp_err_t DlnaHandler::httpDlnaSubscribeHandler(httpd_req_t* req)
{
    EventSrc eventSrc;
    if (strcmp(req->uri, "/dlna/RenderingControl/event") == 0) {
        eventSrc = kEventsRendCtl;
    }
    else if (strcmp(req->uri, "/dlna/AVTransport/event") == 0) {
        eventSrc = kEventsAVTransport;
    }
    else {
        ESP_LOGW(TAG_NOTIFY, "Refusing event subscribe request for '%s'", req->uri);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    printf("subscribe req: '%s'\n", req->uri);
    char bufTout[36];
    uint32_t timeout;
    if (!httpGetHeader(req, "TIMEOUT", [&bufTout](size_t) { return bufTout; }))
    {
        return ESP_FAIL;
    }
    if (strncasecmp(bufTout, "Second-", 7)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No 'second-' prefix in TIMEOUT header");
        return ESP_FAIL;
    }
    const char* strTimeout = bufTout + 7;
    if (strcasecmp(strTimeout, "infinite") == 0) {
        timeout = 0xffffffff;
    } else {
        timeout = parseInt(strTimeout, 0);
        if (!timeout) {
            ESP_LOGW(TAG, "Can't parse timeout, assuming 600 seconds");
            timeout = 600;
        }
    }
    if (timeout > 300) {
        timeout = 300;
    }
    auto& self = *static_cast<DlnaHandler*>(req->user_ctx);
    if (!httpGetHeader(req, "CALLBACK", [&self, eventSrc](size_t urlLen) {
        auto& buf = self.mEventSubs.emplace_front(eventSrc).callbackUrl;
        auto ptr = buf.getAppendPtr(urlLen);
        buf.expandDataSize(urlLen);
        return ptr;
    })) {
        return ESP_FAIL;
    }
    auto& subscr = self.mEventSubs.front();
    auto& url = subscr.callbackUrl;
    // remove <> brackets
    memmove(url.buf(), url.buf()+1, url.dataSize()-3);
    url.setDataSize(url.dataSize()-2);
    url.buf()[url.dataSize()-1] = 0;
    subscr.tsTill = esp_timer_get_time() / 1000000 + timeout;
    vtsnprintf(bufTout, sizeof(bufTout), "Second-", timeout);
    httpd_resp_set_hdr(req, "TIMEOUT", bufTout);
    char bufSid[sizeof(EventSubscription::kSidPrefix) + 10];
    httpd_resp_set_hdr(req, "SID", subscr.strSid(bufSid, sizeof(bufSid)));
    httpd_resp_send(req, nullptr, 0);
    ESP_LOGW(TAG, "Subscribed to event: CALLBACK='%s', SID='%s', timeout='%s'",
        url.buf(), bufSid, bufTout);
    self.mHttpServer.queueWork([&self, sid = subscr.sid]() {
        EventSubscription* subscr = self.eventSubscriptionBySid(sid);
        if (subscr) {
            subscr->notifySubscribed(self.mPlayer);
        }
    });
    return ESP_OK;
}
esp_err_t DlnaHandler::httpDlnaUnsubscribeHandler(httpd_req_t* req)
{
    char strSid[64];
    if (!httpGetHeader(req, "SID", [&strSid](size_t) {
        return strSid;
    })) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error getting SID header");
        return ESP_FAIL;
    }
    uint32_t sid = EventSubscription::parseSid(strSid);
    if (!sid) {
        const char* kMsg = "Error parsing SID header";
        ESP_LOGW(TAG_NOTIFY, "%s '%s'", kMsg, strSid);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, kMsg);
        return ESP_FAIL;
    }
    auto& self = *static_cast<DlnaHandler*>(req->user_ctx);
    int numRemoved = 0;
    self.mEventSubs.remove_if([sid, &numRemoved](EventSubscription& sub) {
        if (sub.sid != sid) {
            return false;
        }
        numRemoved++;
        return true;
    });
    if (!numRemoved) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    else {
        ESP_LOGW(TAG_NOTIFY, "UNSUBSCRIBE: Removed %d subscriptions with sid '%s'", numRemoved, strSid);
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }
}

bool DlnaHandler::registerHttpHandlers()
{
    httpd_uri_t desc = {
        .uri       = "/dlna/*",
        .method    = HTTP_GET,
        .handler   = httpDlnaDescGetHandler,
        .user_ctx  = this
    };
    if (httpd_register_uri_handler(mHttpServer.handle(), &desc) != ESP_OK) {
        goto fail;
    }
    desc.method = HTTP_POST;
    desc.handler = httpDlnaCommandHandler;
    if (httpd_register_uri_handler(mHttpServer.handle(), &desc) != ESP_OK) {
        goto fail;
    }
    desc.method = HTTP_SUBSCRIBE;
    desc.handler = httpDlnaSubscribeHandler;
    if (httpd_register_uri_handler(mHttpServer.handle(), &desc) != ESP_OK) {
        goto fail;
    }
    desc.method = HTTP_UNSUBSCRIBE;
    desc.handler = httpDlnaUnsubscribeHandler;
    if (httpd_register_uri_handler(mHttpServer.handle(), &desc) != ESP_OK) {
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
    httpd_unregister_uri(mHttpServer.handle(), "/dlna");
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
    //printf("sending: %.*s to %s\n", len, mMsgBuf, inet_ntoa(ip));
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
    const char* kErrPrefix = "SSDP request";
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
        ESP_LOGW(TAG, "%s contains no newlines", kErrPrefix);
        return;
    }
    end++;

    KeyValParser params(end, len - (end - str));
    if (!params.parse('\n', ':', KeyValParser::kTrimSpaces | KeyValParser::kKeysToLower)) {
        ESP_LOGW(TAG, "%s: Error parsing HTTP headers of:\n%s\n====", kErrPrefix, end);
        return;
    }
    for (auto& kv: params.keyVals()) {
        auto& val = kv.val;
        if (val.str[val.len-1] == '\r') {
            val.str[--val.len] = 0;
        }
    }
    auto st = params.strVal("st").str;
    if (!st) {
        ESP_LOGW(TAG, "%s: No ST header", kErrPrefix);
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
    else if (strncasecmp(st, kUpnpDeviceSchema, sizeof(kUpnpDeviceSchema) - 1) == 0) {
        st += sizeof(kUpnpDeviceSchema) - 1;
        if (strncasecmp(st, kMediaRenderer, sizeof(kMediaRenderer) - 2) == 0) {
            sendReply(kMediaRenderer, ip, kUpnpDeviceSchema);
        }
    }
    else if (strncasecmp(st, kUpnpServiceSchema, sizeof(kUpnpServiceSchema) - 1) == 0) {
        st += sizeof(kUpnpServiceSchema) - 1;
        if (strncasecmp(st, kAvTransport, sizeof(kAvTransport) - 2) == 0) { // skip the :version part
            sendReply(kAvTransport, ip, kUpnpServiceSchema);
        }
        else if (strncasecmp(st, kRendCtl, sizeof (kRendCtl) - 2) == 0) {
            sendReply(kRendCtl, ip, kUpnpServiceSchema);
        }
        else if (strncasecmp(st, kConnMgr, sizeof(kConnMgr) - 2) == 0) {
            sendReply(kConnMgr, ip, kUpnpServiceSchema);
        }
    }
}
