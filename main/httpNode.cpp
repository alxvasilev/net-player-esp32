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

CodecType HttpNode::codecFromContentType(const char* content_type)
{
    if (strcasecmp(content_type, "mp3") == 0 ||
        strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0 ||
        strcasecmp(content_type, "binary/octet-stream") == 0 ||
        strcasecmp(content_type, "application/octet-stream") == 0) {
        return kCodecMp3;
    }
    else if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
        strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0 ||
        strcasecmp(content_type, "video/MP2T") == 0) {
        return kCodecAac;
    }
    else if (strcasecmp(content_type, "audio/flac") == 0) {
        return kCodecFlac;
    }
    else if (strcasecmp(content_type, "audio/ogg") == 0 ||
             strcasecmp(content_type, "application/ogg") == 0) {
        return kCodecOggTransport;
    }
    else if (strcasecmp(content_type, "audio/wav") == 0) {
        return kCodecWav;
    }
    else if (strcasecmp(content_type, "audio/opus") == 0) {
        return kCodecOpus;
    }
    else if (strcasecmp(content_type, "audio/x-mpegurl") == 0 ||
        strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return kPlaylistM3u8;
    }
    else if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return kPlaylistPls;
    }
    return kCodecUnknown;
}

bool HttpNode::isPlaylist()
{
    if (mInCodec == kPlaylistM3u8 || mInCodec == kPlaylistPls) {
        return true;
    }
    char *dot = strrchr(mUrl, '.');
    return (dot && ((strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)));
}

void HttpNode::doSetUrl(const char *url, const char* recStaName=nullptr)
{
    ESP_LOGI(mTag, "Setting url to %s", url);
    if (mUrl) {
        free(mUrl);
    }
    mUrl = strdup(url);
    if (mRecordingStationName) {
        free(mRecordingStationName);
        mRecordingStationName = nullptr;
    }
    if (recStaName) {
        mRecordingStationName = strdup(recStaName);
    }
    if (mClient) {
        esp_http_client_set_url(mClient, mUrl); // do it here to avoid keeping reference to the old, freed one
    }
}
bool HttpNode::createClient()
{
    assert(!mClient);
    esp_http_client_config_t cfg = {};
    cfg.url = mUrl;
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
    esp_http_client_set_header(mClient, "User-Agent", "curl/7.65.3");
    esp_http_client_set_header(mClient, "Icy-MetaData", "1");
    return true;
}

esp_err_t HttpNode::httpHeaderHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "hdr: '%s': '%s'", evt->header_key, evt->header_value);

    auto self = static_cast<HttpNode*>(evt->user_data);
    auto key = evt->header_key;
    if (strcasecmp(key, "Content-Type") == 0) {
        self->mInCodec = self->codecFromContentType(evt->header_value);
        ESP_LOGI(TAG, "Parsed content-type '%s' as %s", evt->header_value, codecTypeToStr(self->mInCodec));
    } else {
        self->mIcyParser.parseHeader(key, evt->header_value);
    }
    return ESP_OK;
}

bool HttpNode::connect(bool isReconnect)
{
    myassert(mState != kStateStopped);
    if (!mUrl) {
        ESP_LOGE(mTag, "connect: URL has not been set");
        return false;
    }
    ESP_LOGI(mTag, "Connecting to '%s'...", mUrl);
    if (!isReconnect) {
        {
            LOCK();
            mStreamStartPos = mRxByteCtr; // mStreamStartPos is read in pullData()
        }
        recordingMaybeEnable();
    } else {
        recordingCancelCurrent();
    }

    if (!mClient) {
        if (!createClient()) {
            ESP_LOGE(mTag, "connect: Error creating http client");
            return false;
        }
    }
    // request IceCast stream metadata
    mIcyParser.reset();

    esp_http_client_set_header(mClient, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/92.0.4515.159 Safari/537.36");

    if (isReconnect) { // we are resuming, send position
        char rang_header[32];
        snprintf(rang_header, 32, "bytes=%lld-", mRxByteCtr - mStreamStartPos);
        esp_http_client_set_header(mClient, "Range", rang_header);
    }
    plSendEvent(kEventConnecting, isReconnect);

    for (int tries = 0; tries < 26; tries++) {
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
        ESP_LOGI(TAG, "Connected to '%s': http code: %d, content-length: %u",
            mUrl, status_code, mContentLen);

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
        ESP_LOGI(TAG, "Checking if response is a playlist");
        if (parseResponseAsPlaylist()) {
            ESP_LOGI(TAG, "Response parsed as playlist");
            auto url = mPlaylist.getNextTrack();
            if (!url) {
                ESP_LOGE(TAG, "Response is a playlist, but couldn't obtain an url from it");
                return false;
            }
            doSetUrl(url);
            continue;
        }
        if (!mIcyParser.icyInterval()) {
            ESP_LOGW(TAG, "Source does not send ShoutCast metadata");
        }
        plSendEvent(kEventConnected, isReconnect);
        if (!isReconnect) {
            postStreamEvent_Lock(mStreamStartPos, kStreamChanged, mInCodec);
        }
        return true;
    }
    return false;
}
bool HttpNode::parseResponseAsPlaylist()
{
    if (!isPlaylist()) {
        ESP_LOGI(TAG, "Content length and url don't looke like a playlist");
        return false;
    }
    std::unique_ptr<char, decltype(::free)*> buf((char*)malloc(mContentLen), ::free);
    int bufLen = mContentLen;
    for (int retry = 0; retry < 4; retry++) {
        if (!buf.get()) {
            ESP_LOGE(TAG, "Out of memory allocating buffer for playlist download");
            return true; // return empty playlist
        }
        int rlen = esp_http_client_read(mClient, buf.get(), mContentLen);
        if (rlen < 0) {
            disconnect();
            connect();
            if (mContentLen != bufLen) {
                buf.reset((char*)realloc(buf.get(), mContentLen));
            }
            continue;
        }
        buf.get()[rlen] = 0;
        mPlaylist.load(buf.get());
        return true;
    }
    return true;
}

void HttpNode::disconnect()
{
    mPlaylist.clear();
    destroyClient();
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

bool HttpNode::nextTrack()
{
    if (!mAutoNextTrack) {
        return false;
    }
    auto url = mPlaylist.getNextTrack();
    if (!url) {
        return false;
    }
    doSetUrl(url);
    return true;
}

bool HttpNode::recv()
{
    for (int retries = 0; retries < 26; retries++) { // retry net errors
        char* buf;
        auto bufSize = mRingBuf.getWriteBuf(buf, kReadSize, -1);
        if (bufSize < 0) { // stop flag was set
            return false;
        }
        if (bufSize > kReadSize) { // ringbuf will return max possible value, which may be more than the requested
            bufSize = kReadSize;
        }
        int rlen = esp_http_client_read(mClient, buf, bufSize);
        if (rlen <= 0) {
            mRingBuf.abortWrite();
            if (errno == ETIMEDOUT) {
                return false;
            }
            ESP_LOGW(TAG, "Error receiving http stream, errno: %d, contentLen: %d, recv len = %d",
                errno, mContentLen, rlen);
            // even though len == 0 means graceful disconnect, i.e.
            //track end => should go to next track, this often happens when
            // network lags and stream sender aborts sending to us
            // => we should reconnect.
            int msDelay = delayFromRetryCnt(retries);
            ESP_LOGW(TAG, "Reconnecting in %d ms...", msDelay);
            if (mCmdQueue.waitForMessage(msDelay)) {
                return false;
            }
            destroyClient(); // just in case
            connect(true);
            continue;
        }
        LOCK();
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
                    bool ok = mRecorder->onNewTrack(mIcyParser.trackName(), mInCodec);
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
        if (mWaitingPrefill && (ringBufDataSize >= mPrefillAmount)) {
            ESP_LOGI(mTag, "Buffer prefilled >= %d bytes, allowing read", mPrefillAmount);
            setWaitingPrefill(false);
        }
        return true;
    }
    setState(kStatePaused);
    return false;
}

void HttpNode::setUrl(const char* url, const char* recStaName)
{
    if (!mTaskId) {
        doSetUrl(url, recStaName);
    } else {
        ESP_LOGI(mTag, "Posting setUrl command");
        auto urlLen = strlen(url);
        auto staNameLen = recStaName ? strlen(recStaName) : 0;
        char* data = (char*)malloc(urlLen + staNameLen + 2);
        strcpy(data, url);
        data[urlLen] = 0;
        if (recStaName) {
            strcpy(data+urlLen+1, recStaName);
        } else {
            data[urlLen+1] = 0;
        }
        mCmdQueue.post(kCommandSetUrl, data);
    }
}
void HttpNode::pause(bool wait)
{
    // The ring buffer may be full and the thread waiting for space to be freed. If nobody consumes the
    // data, this will result in the thread hanging and not accepting commands. That's why we clear the
    // ringbuffer as well
    AudioNodeWithTask::pause(false);
    mRingBuf.clear();
    if (wait) {
        waitForState(AudioNode::kStatePaused);
    }
}
bool HttpNode::dispatchCommand(Command &cmd)
{
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    switch(cmd.opcode) {
    case kCommandSetUrl: {
        destroyClient();
        const char* url = (const char*)cmd.arg;
        const char* staName = strchr(url, 0) + 1;
        doSetUrl(url, staName);

        free(cmd.arg);
        cmd.arg = nullptr;
        mRingBuf.clear();

        setWaitingPrefill(true);
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
                setState(kStatePaused);
                continue;
            }
        }
        while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
            recv(); // retries and goes to new playlist track. Timeout is for waiting for free space in ringbuf
        }
    }
}

void HttpNode::doStop()
{
    mRingBuf.setStopSignal();
}

HttpNode::~HttpNode()
{
    stop();
    destroyClient();
    free(mUrl);
    free(mRecordingStationName);
}

HttpNode::HttpNode(IAudioPipeline& parent, size_t bufSize, size_t prefillAmount)
    : AudioNodeWithTask(parent, "node-http", kStackSize), mRingBuf(bufSize, utils::haveSpiRam()),
  mPrefillAmount(prefillAmount), mIcyParser(mMutex)
{
}

AudioNode::StreamError HttpNode::pullData(DataPullReq& dp)
{
    while (mWaitingPrefill) {
        ESP_LOGI(TAG, "Waiting ringbuf prefill...");
        if (!waitPrefillChange()) {
            return kStreamStopped;
        }
    }
    {
        // First, process stream events that are due
        LOCK();
        while (!mStreamEventQueue.empty()) {
            auto& event = *mStreamEventQueue.front();
            auto eventPos = event.streamPos;
            auto readPos = mRxByteCtr - mRingBuf.dataSize();
            if (eventPos <= readPos) {
                decltype(mStreamEventQueue)::Popper popper(mStreamEventQueue);
                if (event.type == kStreamChanged) {
                    this->mOutCodec = event.codec;
                    dp.codec = event.codec;
                    dp.size = 0;
                    return kStreamChanged;
                } else if (event.type == kTitleChanged) {
                    ESP_LOGI(TAG, "Sending title event '%s'", (const char*)event.data);
                    plSendEvent(kEventTrackInfo, (uintptr_t)event.data);
                    continue;
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
    }
    if (dp.size == 0) { // there was no due stream change event, and this is a codec probe
        assert(mOutCodec); // waiting for prefill guarantees we already have a stream
        dp.codec = mOutCodec;
        return kNoError;
    }
    auto ret = mRingBuf.contigRead(dp.buf, dp.size, -1);
    if (ret < 0) {
        dp.size = 0;
        return kStreamStopped;
    }
    myassert(ret > 0); // no timeout
    dp.size = ret;
    return kNoError;
}

void HttpNode::confirmRead(int size)
{
    mRingBuf.commitContigRead(size);
}

void HttpNode::setWaitingPrefill(bool prefill)
{
    mWaitingPrefill = prefill;
    mEvents.setBits(kEvtPrefillChange);
}

bool HttpNode::waitPrefillChange()
{
    auto bits = mEvents.waitForOneAndReset(kEvtPrefillChange|kEvtStopRequest, -1);
    if (bits & kEvtStopRequest) {
        return false;
    } else {
        myassert(bits & kEvtPrefillChange);
        return true;
    }
}

bool HttpNode::recordingMaybeEnable() {
    LOCK();
    if (!mRecordingStationName) {
        return false;
    }
    if (!mRecorder) {
        mRecorder.reset(new TrackRecorder("/sdcard/rec"));
    }
    mRecorder->setStation(mRecordingStationName);
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
    if (mRecordingStationName) {
        free(mRecordingStationName);
        mRecordingStationName = nullptr;
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

int HttpNode::delayFromRetryCnt(int tries) {
    if (tries < 4) {
        return 0;
    }
    if (tries < 10) {
        return tries * 100;
    }
    int delay = tries * 1000;
    return (delay <= 30000) ? delay : 30000;
}
