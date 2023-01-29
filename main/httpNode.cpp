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
    else if (strcasecmp(content_type, "audio/wav") == 0) {
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
    setWaitingPrefill(0);
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
        ESP_LOGI(TAG, "Parsed content-type '%s' as %s", val, mInFormat.codec().toString());
    }
    else if ((strcasecmp(key, "accept-ranges") == 0) && (strcasecmp(val, "bytes") == 0)) {
        mAcceptsRangeRequests = true;
    }
    else {
        mIcyParser.parseHeader(key, val);
    }
}
void HttpNode::clearRingBufAndEventQueue()
{
    ESP_LOGI(TAG, "Clearing ring buffer and event queue");
    mRingBuf.clear();
    mStreamEventQueue.clear();
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
            clearRingBufAndEventQueue(); // clear in case of hard reconnect
            mStreamStartPos = mRxByteCtr; // mStreamStartPos is read in pullData()
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
        vtsnprintf(strbuf, sizeof(strbuf), "bytes=", mRxByteCtr - mStreamStartPos, '-');
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
            postStreamEvent_Lock(mStreamStartPos, kStreamChanged, mInFormat, mUrlInfo->streamId);
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

bool HttpNode::recv()
{
    for (int retries = 0; retries < 26; retries++) { // retry net errors
        char* buf;
        auto bufSize = mRingBuf.getWriteBuf(buf, kReadSize, kHttpRecvTimeoutMs);
        if (bufSize <= 0) { // stop flag was set, or timeout
            if (bufSize == 0) {
                ESP_LOGW(TAG, "Ringbuf write timeout, consumer node is probably stuck");
            }
            return false;
        }
        if (bufSize > kReadSize) { // ringbuf will return max possible value, which may be more than the requested
            bufSize = kReadSize;
        }
        int rlen = esp_http_client_read(mClient, buf, bufSize);
        if (rlen <= 0) {
            mRingBuf.abortWrite();
            if (rlen == 0) {
                if (mContentLen && esp_http_client_is_complete_data_received(mClient)) {
                    // transfer complete, post end of stream
                    ESP_LOGI(TAG, "Transfer complete, posting kStreamEnd event (streamId=%d)", mUrlInfo->streamId);
                    postStreamEvent_Lock(mRxByteCtr, kStreamEnd, mUrlInfo->streamId);
                    return false;
                }
                else if (errno == EAGAIN) { // just return, main loop will retry
                    return true;
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
                return false;
            }
            destroyClient(); // just in case
            connect(canResume());
            continue;
        }
        LOCK();
        mSpeedProbe.onTraffic(rlen);
        if (mIcyParser.icyInterval()) {
            bool isFirst = !mIcyParser.trackName();
            bool gotTitle = mIcyParser.processRecvData(buf, rlen);
            if (gotTitle) {
                ESP_LOGW(TAG, "Track title changed to: '%s'", mIcyParser.trackName());
                postStreamEvent_NoLock(
                    isFirst ? mStreamStartPos : (mRxByteCtr + rlen - mIcyParser.bytesSinceLastMeta()),
                    kTitleChanged, strdup(mIcyParser.trackName())
                );

                if (mRecorder && !isFirst) { // start recording only on second icy track event - first track may be incomplete
                    bool ok = mRecorder->onNewTrack(mIcyParser.trackName(), mInFormat.codec());
                    plSendEvent(kEventRecording, ok);
                }
            }
        }
        mRingBuf.commitWrite(rlen);
        mRxByteCtr += rlen;
        // First commit the write, only after that record to SD card,
        // to avoid blocking the stream consumer
        // Note: The buffer is still valid, even if it has been consumed
        // before we reach the next line - ringbuf consumers are read-only
        if (mRecorder) {
            mRecorder->onData(buf, rlen);
        }
        auto ringBufDataSize = mRingBuf.dataSize();
        ESP_LOGD(TAG, "Received %d bytes, wrote to ringbuf (%d)", rlen, ringBufDataSize);
        if (mWaitingPrefill && (ringBufDataSize >= mWaitingPrefill)) {
            ESP_LOGI(mTag, "Buffer prefilled >= %d bytes, allowing read", mWaitingPrefill);
            setWaitingPrefill(0);
            plSendEvent(kEventPlaying);
        }
        return true;
    }
    return false;
}
void HttpNode::logStartOfRingBuf(const char* msg)
{
    char hex[128];
    char* tmpbuf = nullptr;
    auto nread = mRingBuf.contigRead(tmpbuf, 20, -1);
    if (nread <= 0) {
        hex[0] = 0;
    } else {
        binToHex((uint8_t*)tmpbuf, nread, hex);
    }
    printf("%s: %s\n", msg, hex);
    mRingBuf.commitContigRead(0);
}
uint32_t HttpNode::pollSpeed() const
{
    LOCK();
    return mSpeedProbe.poll();
//  return mSpeedProbe.average();
}

void HttpNode::setUrl(UrlInfo* urlInfo)
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
        clearRingBufAndEventQueue();
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
            if (!recv()) { // returns false on error or end of stream
                stop(false);
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

HttpNode::HttpNode(IAudioPipeline& parent, size_t bufSize)
    : AudioNodeWithTask(parent, "node-http", kStackSize, 5, 1),
      mRingBuf(bufSize, utils::haveSpiRam()), mIcyParser(mMutex)
{
}

void HttpNode::setUnderrunState(bool newState)
{
    if (newState == mBufUnderrunState) {
        return;
    }
    mBufUnderrunState = newState;
    if (newState) {
        plSendEvent(kEventBufState);
    }
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
AudioNode::StreamError HttpNode::pullData(DataPullReq& dp)
{
    if (mWaitingPrefill) {
        ESP_LOGI(TAG, "Waiting ringbuf prefill...");
        if (!waitForPrefill()) {
            dp.clear();
            dp.streamId = currentStreamId();
            return kStreamStopped;
        }
    }
    LOCK(); // we may have been pre-filled, but cleared just before the lock, so check once again after locking
    auto dataSize = mRingBuf.dataSize();
    if (dataSize) {
        setUnderrunState(false);
    } else {
        auto evt = dequeueStreamEvent(dp);
        if (evt != kNoError) {
            return evt; // ringbuf is empty but there is an event, probably kStreamEnd
        }
        setUnderrunState(true);
        int waitResult;
        {
            MutexUnlocker unlock(mMutex);
            waitResult = mRingBuf.waitForData(-1);
        }
        if (waitResult < 0) {
            dp.clear();
            dp.streamId = currentStreamId();
            return kStreamStopped;
        }
    }
    // First, process stream events that are due
    auto evt = dequeueStreamEvent(dp);
    if (evt != kNoError) {
        return evt;
    }
    dp.fmt = mOutFormat;
    if (dp.size == 0) { // there was no due stream change event, and this is a codec probe
        return kNoError;
    }
    //printf("ringbuf: %d\n", mRingBuf.dataSize());
    auto ret = mRingBuf.contigRead(dp.buf, dp.size, 0);
    if (ret < 0) {
        dp.size = 0;
        dp.streamId = currentStreamId();
        return kStreamStopped;
    }
    myassert(ret > 0); // no timeout
    dp.size = ret;
    return kNoError;
}
const char* HttpNode::peek(int len, char* buf)
{
    LOCK();
    while (mRingBuf.dataSize() < len) {
        MutexUnlocker unlocker(mMutex);
        if (mRingBuf.waitForData(-1) <= 0) {
            return nullptr;
        }
    }
    if (!mStreamEventQueue.empty()) {
        bool hasEvent = false;
        mStreamEventQueue.iterate([this, &hasEvent, len](const QueuedStreamEvent& event) {
            auto eventPos = event.streamPos;
            auto readPos = mRxByteCtr - mRingBuf.dataSize();
            if (eventPos < readPos + len) {
                if (event.type == kTitleChanged) {
                    return true; // skip
                }
                hasEvent = true;
            }
            return false;
        });
        if (hasEvent) {
            return nullptr;
        }
    }
    return mRingBuf.peek(len, buf, 0);
}

AudioNode::StreamError HttpNode::dequeueStreamEvent(DataPullReq& dp)
{
    while (!mStreamEventQueue.empty()) {
        auto& event = *mStreamEventQueue.front();
        auto eventPos = event.streamPos;
        auto readPos = mRxByteCtr - mRingBuf.dataSize();
        if (eventPos <= readPos) {
            decltype(mStreamEventQueue)::Popper popper(mStreamEventQueue);
            if (event.type == kStreamChanged) {
                dp.fmt = mOutFormat = event.fmt;
                dp.streamId = event.streamId;
                dp.size = 0;
                ESP_LOGI(TAG, "Returning kStreamChanged: codec %s, streamId: %u", event.fmt.codec().toString(), dp.streamId);
                return kStreamChanged;
            }
            else if (event.type == kTitleChanged) {
                plSendEvent(kEventTrackInfo, 0, (uintptr_t)event.data);
                continue;
            }
            else if(event.type == kStreamEnd) {
                dp.clear();
                dp.streamId = event.streamId;
                return kStreamEnd;
            }
        } else if (eventPos < readPos + dp.size) { // there is an event within the read window
            // If we were called with zero size for a format probe, the previous 'if' will handle it
            myassert(dp.size > 0);
            dp.size = eventPos - readPos;
            break;
        } else {
            break;
        }
    }
    return kNoError;
}

void HttpNode::confirmRead(int size)
{
    mRingBuf.commitContigRead(size);
}

void HttpNode::setWaitingPrefill(int amount)
{
    if (amount && mRingBuf.dataSize() >= amount) {
        return;
    }
    if (amount) {
        ESP_LOGI(TAG, "Setting required buffer prefill to %d bytes", amount);
    }
    mWaitingPrefill = amount;
    mEvents.setBits(kEvtPrefillChange);
}

bool HttpNode::waitForPrefill()
{
    while (mWaitingPrefill) {
        auto bits = mEvents.waitForOneAndReset(kEvtPrefillChange|kEvtStopRequest, -1);
        if (bits & kEvtStopRequest) {
            return false;
        } else {
            myassert(bits & kEvtPrefillChange);
        }
    }
    return true;
}

bool HttpNode::recordingMaybeEnable() {
    auto staName = recStaName();
    if (!staName) {
        return false;
    }
    ESP_LOGW(TAG, "============Preparing recorder for station %s", staName);
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
