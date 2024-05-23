#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "httpNode.hpp"

#define LOCK() MutexLocker locker(mMutex)

static const char *TAG = "node-http";
StreamFormat HttpNode::codecFromContentType(const char* content_type)
{
    if (strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0) {
        return Codec::kCodecMp3;
    }
    else if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
//      strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0) {
        return Codec::kCodecAac;
    }
    else if (strcasecmp(content_type, "audio/flac") == 0 ||
             strcasecmp(content_type, "audio/x-flac") == 0) {
        return Codec::kCodecFlac;
    }
    else if (strcasecmp(content_type, "audio/ogg") == 0 ||
             strcasecmp(content_type, "application/ogg") == 0) {
        return Codec(Codec::kCodecUnknown, Codec::kTransportOgg);
    }
    else if (strcasecmp(content_type, "audio/wav") == 0 ||
             strcasecmp(content_type, "audio/x-wav") == 0) {
        return Codec::kCodecWav;
    }
    else if (strncasecmp(content_type, "audio/L16", 9) == 0) {
        return parseLpcmContentType(content_type, 16);
    }
    else if (strncasecmp(content_type, "audio/L24", 9) == 0) {
        return parseLpcmContentType(content_type, 24);
    }
    else if (strcasecmp(content_type, "audio/opus") == 0) {
        return Codec::kCodecOpus;
    }
    else if (strcasecmp(content_type, "audio/x-mpegurl") == 0 ||
        strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return Codec::kPlaylistM3u8;
    }
    else if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return Codec::kPlaylistPls;
    }
    return Codec::kCodecUnknown;
}
StreamFormat HttpNode::parseLpcmContentType(const char* ctype, int bps)
{
    const char* kMsg = "Error parsing audio/Lxx";
    ctype = strchr(ctype, ';');
    if (!ctype) {
        ESP_LOGW(TAG, "%s: No semicolon found", kMsg);
        return Codec::kCodecUnknown;
    }
    ctype++;
    auto len = strlen(ctype) + 1;
    auto copy = (char*)malloc(len);
    memcpy(copy, ctype, len);
    KeyValParser params(copy, len, true);
    if (!params.parse(';', '=', KeyValParser::kTrimSpaces)) {
        ESP_LOGW(TAG, "%s params", kMsg);
        return Codec::kCodecUnknown;
    }
    auto sr = params.intVal("rate", 0);
    if (sr == 0) {
        ESP_LOGW(TAG, "%s samplerate", kMsg);
        return Codec::kCodecUnknown;
    }
    auto chans = params.intVal("channels", 1);
    StreamFormat fmt(Codec::kCodecPcm, sr, 16, chans);
    auto endian = params.strVal("endianness");
    if (!endian || strcasecmp(endian.str, "little-endian") != 0) {
        fmt.setBigEndian(true);
    }
    return fmt;
}
bool HttpNode::isPlaylist()
{
    if (mInFormat.codec().type == Codec::kPlaylistM3u8 || mInFormat.codec().type == Codec::kPlaylistPls) {
        return true;
    }
    char *dot = strrchr(url(), '.');
    return (dot && ((strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)));
}

void HttpNode::doSetUrl(UrlInfo* urlInfo)
{
    ESP_LOGI(mTag, "Setting url to %s", urlInfo->url);
    if (mClient) {
        esp_http_client_set_url(mClient, urlInfo->url); // do it here to avoid keeping reference to the old, freed one
    }
    mUrlInfo.reset(urlInfo);
}
void HttpNode::updateUrl(const char* url)
{
    auto urlInfo = mUrlInfo.get()
        ? UrlInfo::Create(url, mUrlInfo->streamId, mUrlInfo->recStaName)
        : UrlInfo::Create(url, 0, nullptr);
    doSetUrl(urlInfo);
}
bool HttpNode::createClient()
{
    assert(!mClient);
    esp_http_client_config_t cfg = {};
    cfg.url = url();
    cfg.event_handler = httpHeaderHandler;
    cfg.user_data = this;
    cfg.timeout_ms = kHttpRecvTimeoutMs;
    cfg.buffer_size = kHttpClientBufSize;
    cfg.method = HTTP_METHOD_GET;

    mClient = esp_http_client_init(&cfg);
    if (!mClient)
    {
        ESP_LOGE(TAG, "Error creating http client, probably out of memory");
        return false;
    };
    esp_http_client_set_header(mClient, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/92.0.4515.159 Safari/537.36");
    esp_http_client_set_header(mClient, "Icy-MetaData", "1");
    return true;
}

esp_err_t HttpNode::httpHeaderHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "\e[34mhdr: '%s': '%s'", evt->header_key, evt->header_value);

    static_cast<HttpNode*>(evt->user_data)->onHttpHeader(evt->header_key, evt->header_value);
    return ESP_OK;
}
void HttpNode::onHttpHeader(const char* key, const char* val)
{
    if (strcasecmp(key, "Content-Type") == 0) {
        mInFormat = codecFromContentType(val);
        mNetRecvSize = mInFormat.netRecvSize();
        ESP_LOGI(TAG, "Parsed content-type '%s' as %s, recv chunk size set to %d",
            val, mInFormat.codec().toString(), mNetRecvSize);
    }
    else if ((strcasecmp(key, "accept-ranges") == 0) && (strcasecmp(val, "bytes") == 0)) {
        mAcceptsRangeRequests = true;
    }
    else {
        mIcyParser.parseHeader(key, val);
    }
}
void HttpNode::clearRingBuffer()
{
    ESP_LOGI(TAG, "Clearing ring buffer");
    mRingBuf.clear();
    mSpeedProbe.reset();
}
bool HttpNode::connect(bool isReconnect)
{
    if (state() != kStateRunning) {
        ESP_LOGW(TAG, "connect: soft assert: not in kStateRunning, but in state %s", stateToStr(state()));
        return false;
    }
    if (!url()) {
        ESP_LOGE(mTag, "connect: URL has not been set");
        return false;
    }
    ESP_LOGI(mTag, "Connecting to '%s'...", url());
    {
        LOCK();
        if (!isReconnect) {
            mAcceptsRangeRequests = false;
            clearRingBuffer(); // clear in case of hard reconnect
            mStreamByteCtr = 0;
            mWaitingPrefill = 0;
        }
        recordingMaybeEnable();
    }

    if (!mClient) {
        if (!createClient()) {
            ESP_LOGE(mTag, "connect: Error creating http client");
            return false;
        }
    }
    // request IceCast stream metadata
    mIcyParser.reset();

    if (isReconnect) { // we are resuming, send position
        char strbuf[32];
        vtsnprintf(strbuf, sizeof(strbuf), "bytes=", mStreamByteCtr, '-');
        esp_http_client_set_header(mClient, "Range", strbuf);
    }
    plSendEvent(kEventConnecting, isReconnect);

    for (int tries = 0; tries < 10; tries++) {
        auto err = esp_http_client_open(mClient, 0);
        if (err != ESP_OK) {
            int msDelay = delayFromRetryCnt(tries);
            ESP_LOGE(TAG, "Failed to open http stream, error %s. Will retry in %d ms", esp_err_to_name(err), msDelay);
            if (mCmdQueue.waitForMessage(msDelay)) {
                ESP_LOGD(mTag, "Command received while delaying retry, aborting");
                return false;
            }
            continue;
        }

        mContentLen = esp_http_client_fetch_headers(mClient);
        int status_code = esp_http_client_get_status_code(mClient);
        ESP_LOGI(TAG, "Connected to '%s': http code: %d, content-length: %d", url(), status_code, mContentLen);

        if (status_code < 0) {
            auto msDelay = delayFromRetryCnt(tries);
            ESP_LOGE(mTag, "Negative http status code while connecting, will retry in %d ms", msDelay);
            if (mCmdQueue.waitForMessage(msDelay)) {
                ESP_LOGD(mTag, "Command received while delaying retry, aborting");
                return false;
            }
            continue;
        }
        else if (status_code == 301 || status_code == 302) {
            ESP_LOGI(TAG, "Following redirect...");
            esp_http_client_set_redirection(mClient);
            continue;
        }
        else if (status_code != 200 && status_code != 206) {
            ESP_LOGE(mTag, "Non-200 response code %d", status_code);
            plSendError(kErrNotFound, 0);
            return false;
        }
        auto ret = handleResponseAsPlaylist(mContentLen);
        if (ret) {
            if (ret <= -2) {
                return false;
            } else { // retriable network error (-1) or is a playlist and url was updated(> 0)
                continue;
            }
        }
        if (!mIcyParser.icyInterval()) {
            ESP_LOGW(TAG, "Source does not send ShoutCast metadata");
        }
        plSendEvent(kEventConnected, isReconnect);
        if (!isReconnect) {
            ESP_LOGD(TAG, "Posting kStreamChange with codec %s and streamId %d\n", mInFormat.codec().toString(), mUrlInfo->streamId);
            mRingBuf.pushBack(new GenericEvent(kEvtStreamChanged, mUrlInfo->streamId, mInFormat));
        }
        return true;
    }
    return false;
}
int8_t HttpNode::handleResponseAsPlaylist(int32_t contentLen)
{
    if (!isPlaylist()) {
        return 0;
    }
    ESP_LOGI(TAG, "Response looks like a playlist");
    if (!contentLen) {
        contentLen = 2048;
    }
    std::unique_ptr<char[]> buf(new char[contentLen + 1]);
    if (!buf.get()) {
        ESP_LOGE(TAG, "Out of memory allocating buffer for playlist download");
        return -2;
    }
    int rlen = esp_http_client_read(mClient, buf.get(), contentLen);
    if (rlen < 0) {
        ESP_LOGW(TAG, "Error %d receiving playlist, retrying...", esp_http_client_get_errno(mClient));
        return -1;
    }
    buf.get()[rlen] = 0;
    mPlaylist.load(buf.get());
    auto url = mPlaylist.getNextTrack();
    if (!url) {
        ESP_LOGE(TAG, "Response is a playlist, but couldn't obtain an url from it");
        return -2;
    }
    updateUrl(url);
    return 1;
}

void HttpNode::destroyClient()
{
    if (!mClient) {
        return;
    }
    esp_http_client_close(mClient);
    esp_http_client_cleanup(mClient);
    mClient = NULL;
}

bool HttpNode::isConnected() const
{
    return mClient != nullptr;
}
/* Returns 0 on success, 1 if stream ended (node should not stop, just wait for a new command),
   and -1 on error - node should stop
*/
int8_t HttpNode::recv()
{
    for (int retries = 0; retries < 26; retries++) { // retry net errors
        DataPacket::unique_ptr dataPacket(DataPacket::create(mNetRecvSize));
        int rlen = esp_http_client_read(mClient, dataPacket->data, mNetRecvSize);
        if (rlen <= 0) {
            mWaitingPrefill = 0;
            if (rlen == 0) {
                if (mContentLen && esp_http_client_is_complete_data_received(mClient)) {
                    // transfer complete, post end of stream
                    ESP_LOGI(TAG, "Transfer complete, posting kStreamEnd event (streamId=%d)", mUrlInfo->streamId);
                    mRingBuf.pushBack(new GenericEvent(kEvtStreamEnd, mUrlInfo->streamId, 0));
                    return 1; // don't stop the node, just wait for new commands
                }
                else if (errno == EAGAIN) {
                    return 0; // just return, main loop will retry
                }
                // else we have a graceful disconnect but incomplete transfer?
            }
            recordingCancelCurrent();
            ESP_LOGW(TAG, "Error '%s' receiving http stream, returned rlen: %d", strerror(errno), rlen);
            // even though len == 0 means graceful disconnect, this often happens when
            // network lags and stream sender aborts sending to us => we should reconnect.
            int msDelay = delayFromRetryCnt(retries);
            ESP_LOGW(TAG, "Reconnecting in %d ms...", msDelay);
            if (mCmdQueue.waitForMessage(msDelay)) {
                return 1; // main loop will process the command, but we won't reconnect
            }
            destroyClient(); // just in case
            connect(canResume());
            continue;
        }
        LOCK();
        mSpeedProbe.onTraffic(rlen);
        if (mIcyParser.icyInterval()) {
            bool isFirst = !mIcyParser.trackName();
            bool gotTitle = mIcyParser.processRecvData(dataPacket->data, rlen);
            if (gotTitle) {
                ESP_LOGW(TAG, "Track title changed to: '%s'", mIcyParser.trackName());
                mRingBuf.pushBack(TitleChangeEvent::create(mIcyParser.trackName()));
                // offset of title within packet: (rlen - mIcyParser.bytesSinceLastMeta())

                if (mRecorder && !isFirst) { // start recording only on second icy track event - first track may be incomplete
                    bool ok = mRecorder->onNewTrack(mIcyParser.trackName(), mInFormat.codec());
                    plSendEvent(kEventRecording, ok);
                }
            }
        }
        dataPacket->dataLen = rlen;
        mStreamByteCtr += rlen;
        if (mRecorder) {
            mRecorder->onData(dataPacket->data, dataPacket->dataLen);
        }
        {
            MutexUnlocker unlocker(mMutex);
            mRingBuf.pushBack(dataPacket.release());
        }
        auto ringBufDataSize = mRingBuf.dataSize();
        ESP_LOGD(TAG, "Received %d bytes, wrote to ringbuf (%d)", rlen, ringBufDataSize);
        return 0;
    }
    plSendError(kErrStreamStopped, 0);
    return -1;
}
uint32_t HttpNode::pollSpeed() const
{
    LOCK();
    return mSpeedProbe.poll();
//  return mSpeedProbe.average();
}

void HttpNode::setUrlAndStart(UrlInfo* urlInfo)
{
    if (!mTaskId) {
        run();
    }
    ESP_LOGI(mTag, "Posting setUrl command");
    mCmdQueue.post(kCommandSetUrl, urlInfo);
}

void HttpNode::onStopRequest()
{
    mRingBuf.setStopSignal();
}

bool HttpNode::dispatchCommand(Command &cmd)
{
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    switch(cmd.opcode) {
    case kCommandSetUrl: {
        destroyClient();
        doSetUrl((UrlInfo*)cmd.arg);
        // must be cleared before going into kStateRunning because player will start the output node
        // which will start reading from us. Before we had a prefill set here, which guaranteed some delay
        // to clear the ringbuf in connect(). Now we don't have to wait prefill for the first audio packet
        clearRingBuffer();
        mRingBuf.clearStopSignal();
        cmd.arg = nullptr;
        setState(kStateRunning);
        break;
    }
    default:
        return false;
    }
    return true;
}

void HttpNode::nodeThreadFunc()
{
    ESP_LOGI(TAG, "Task started");
    mRingBuf.clearStopSignal();
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        if (!isConnected()) {
            if (!connect()) {
                stop(false);
                continue;
            } else {
                ESP_LOGI(TAG, "Buffering...");
            }
        }
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            auto ret = recv();
            if (ret) { // returns nonzero on error or end of stream
                if (ret < 0) {
                    stop(false);
                }
                break;
            }
        }
    }
}

HttpNode::~HttpNode()
{
    terminate(true);
    destroyClient();
}

HttpNode::HttpNode(IAudioPipeline& parent)
    : AudioNodeWithTask(parent, "node-http", kStackSize, 15, 0), mIcyParser(mMutex)
{
}

void HttpNode::setUnderrunState(bool isUnderrun)
{
    if (isUnderrun == mIsBufUnderrun) {
        return;
    }
    mIsBufUnderrun = isUnderrun;
    if (isUnderrun) {
        ESP_LOGW(TAG, "Underrun");
    }
    plSendEvent(kEventBufUnderrun, isUnderrun);
}
uint32_t HttpNode::currentStreamId() const
{
    if (mUrlInfo.get()) {
        return mUrlInfo->streamId;
    } else {
        ESP_LOGW(mTag, "Assert fail: Returning zero streamId for event, as we have no urlInfo");
        return 0;
    }
}
StreamEvent HttpNode::pullData(PacketResult& pr)
{
    LOCK();
    if (mWaitingPrefill && mPrefillSentFirstData) {
        ESP_LOGI(TAG, "Waiting buffer prefill...");
        while (mStreamByteCtr < mWaitingPrefill) {
            MutexUnlocker unlocker(mMutex);
            auto ret = mRingBuf.waitForWriteOp(-1);
            if (ret < 0) {
                return kErrStreamStopped;
            }
        }
        mWaitingPrefill = 0;
    }
    setUnderrunState(!mRingBuf.dataSize());
    StreamPacket::unique_ptr pkt;
    {
        MutexUnlocker unlocker(mMutex);
        pkt.reset(mRingBuf.popFront());
    }
    if (!pkt) {
        pr.streamId = currentStreamId();
        return kErrStreamStopped;
    }
    if (pkt->type == kEvtStreamChanged) {
        auto& pktChange = static_cast<GenericEvent&>(*pkt);
        mOutStreamId = pktChange.streamId;
        mWaitingPrefill = pktChange.fmt.prefillAmount();
        mPrefillSentFirstData = false;
        ESP_LOGI(TAG, "Returning start of new stream, setting prefill to %d bytes", mWaitingPrefill);
    }
    else if (pkt->type == kEvtData) {
        mPrefillSentFirstData = true;
    }
    return pr.set(pkt);
}
DataPacket* HttpNode::peekData(bool& preceded)
{
    return mRingBuf.peekFirstDataWait(kEvtTitleChanged, &preceded);
}
StreamPacket* HttpNode::peek() {
    return mRingBuf.peekFirstWait();
}
void HttpNode::streamFormatDetails(StreamFormat fmt) {
    auto rxSize = fmt.netRecvSize();
    LOCK();
    if (mNetRecvSize != rxSize) {
        mNetRecvSize = rxSize;
        ESP_LOGI(TAG, "Updated network recv size for codec %s to %d", fmt.codec().toString(), mNetRecvSize);
    }
    if (mWaitingPrefill) {
        mWaitingPrefill = fmt.prefillAmount();
    }
}
bool HttpNode::recordingMaybeEnable() {
    auto staName = recStaName();
    if (!staName) {
        return false;
    }
    ESP_LOGW(TAG, "Preparing recorder for station %s", staName);
    if (!mRecorder) {
        mRecorder.reset(new TrackRecorder("/sdcard/rec"));
    }
    mRecorder->setStation(staName);
    plSendEvent(kEventRecording, true);
    return true;
}

void HttpNode::recordingCancelCurrent() {
    LOCK();
    if (!mRecorder) {
        return;
    }
    mRecorder->abortTrack();
    plSendEvent(kEventRecording, false);
}
void HttpNode::recordingStop() {
    LOCK();
    mRecorder.reset();
    if (recStaName()) {
        mUrlInfo->recStaName = nullptr;
    }
    plSendEvent(kEventRecording, false);
}
bool HttpNode::recordingIsActive() const
{
    LOCK();
    return mRecorder && mRecorder->isRecording();
}
bool HttpNode::recordingIsEnabled() const
{
    LOCK();
    return mRecorder.get() != nullptr;
}

int HttpNode::delayFromRetryCnt(int tries)
{
    return (tries < 2) ? 0 : tries * 1000;
}
