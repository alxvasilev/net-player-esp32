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

static const char *TAG = "HTTP_NODE";

CodecType HttpNode::codecFromContentType(const char* content_type)
{
    if (strcasecmp(content_type, "mp3") == 0 ||
        strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0 ||
        strcasecmp(content_type, "binary/octet-stream") == 0 ||
        strcasecmp(content_type, "application/octet-stream") == 0) {
        return kCodecMp3;
    }
    if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
        strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0 ||
        strcasecmp(content_type, "video/MP2T") == 0) {
        return kCodecAac;
    }
    if (strcasecmp(content_type, "application/ogg") == 0) {
        return kCodecOgg;
    }
    if (strcasecmp(content_type, "audio/wav") == 0) {
        return kCodecWav;
    }
    if (strcasecmp(content_type, "audio/opus") == 0) {
        return kCodecOpus;
    }
    if (strcasecmp(content_type, "audio/x-mpegurl") == 0 ||
        strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return kPlaylistM3u8;
    }
    if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return kPlaylistPls;
    }
    return kCodecUnknown;
}

bool HttpNode::isPlaylist()
{
    auto codec = mStreamFormat.codec;
    if (codec == kPlaylistM3u8 || codec == kPlaylistPls) {
        return true;
    }
    char *dot = strrchr(mUrl, '.');
    return (dot && ((strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)));
}

void HttpNode::doSetUrl(const char *url)
{
    ESP_LOGI(mTag, "Setting url to %s", url);
    if (mUrl) {
        free(mUrl);
    }
    mUrl = strdup(url);
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
    cfg.timeout_ms = kPollTimeoutMs;
    cfg.buffer_size = kClientBufSize;
    cfg.method = mIsWriter ? HTTP_METHOD_POST : HTTP_METHOD_GET;

    mClient = esp_http_client_init(&cfg);
    if (!mClient)
    {
        ESP_LOGE(TAG, "Error creating http client, probably out of memory");
        return false;
    };
    return true;
}
esp_err_t HttpNode::httpHeaderHandler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    if (strcasecmp(evt->header_key, "Content-Type")) {
        return ESP_OK;
    }
    auto self = static_cast<HttpNode*>(evt->user_data);
    self->mStreamFormat.codec = self->codecFromContentType(evt->header_value);
    ESP_LOGI(TAG, "Parsed content-type '%s' as %d", evt->header_value, self->mStreamFormat.codec);
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
        ESP_LOGI(mTag, "connect: Waiting for buffer to drain...");
        // Wait till buffer is drained before changing format descriptor
        if (mWaitingPrefill && mRingBuf.hasData()) {
            ESP_LOGW(mTag, "Connect: Read state is kReadPrefill, but the buffer should be drained, allowing read");
            setWaitingPrefill(false);
        }
        bool ret = mRingBuf.waitForEmpty();
        if (!ret) {
            return false;
        }
        mStreamFormat.reset();
        mBytePos = 0;
    }

    if (!mClient) {
        if (!createClient()) {
            ESP_LOGE(mTag, "connect: Error creating http client");
            return false;
        }
    }

    if (mBytePos) { // we are resuming, send position
        char rang_header[32];
        snprintf(rang_header, 32, "bytes=%lld-", mBytePos);
        esp_http_client_set_header(mClient, "Range", rang_header);
    }
    sendEvent(kEventOnRequest, mClient, 0);

    if (mIsWriter) {
        return esp_http_client_open(mClient, -1); // -1 for content length means chunked encoding
    }
    for (int tries = 0; tries < 4; tries++) {
        auto err = esp_http_client_open(mClient, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open http stream, error %s", esp_err_to_name(err));
            return false;
        }

        int64_t contentLen = esp_http_client_fetch_headers(mClient);
        if (!mBytePos) {
            mBytesTotal = contentLen;
        }

        int status_code = esp_http_client_get_status_code(mClient);
        ESP_LOGI(TAG, "Connected to '%s': http code: %d, content-length: %d",
            mUrl, status_code, (int)mBytesTotal);

        if (status_code == 301 || status_code == 302) {
            ESP_LOGI(TAG, "Following redirect...");
            esp_http_client_set_redirection(mClient);
            continue;
        }
        else if (status_code != 200 && status_code != 206) {
            if (status_code < 0) {
                ESP_LOGE(mTag, "Error connecting, will retry");
                continue;
            }
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
    int plLen = 0;
    std::unique_ptr<char, decltype(::free)*> buf(nullptr, ::free);
    int rlen;
    for (int retry = 0; retry < 4; retry++) {
        if (mBytesTotal != plLen) {
            plLen = mBytesTotal;
            auto ptr = buf.release();
            buf.reset((char*)(ptr ? realloc(ptr, plLen + 1) : malloc(plLen + 1)));
            if (!buf.get()) {
                ESP_LOGE(TAG, "Out of memory allocating buffer for playlist download");
                return true;
            }
        }
        rlen = esp_http_client_read(mClient, buf.get(), plLen);
        if (rlen < 0) {
            disconnect();
            connect();
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
    if (mIsWriter) {
        esp_http_client_fetch_headers(mClient);
    }
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

void HttpNode::recv()
{
    for(;;) { // retry with next playlist track
        for (int retries = 0; retries < 4; retries++) { // retry net errors
            char* buf;
            auto bufSize = mRingBuf.getWriteBuf(buf, kReadSize);

            if (bufSize < 0) { // command queued
                return;
            } else if (bufSize > mRecvSize) {
                bufSize = mRecvSize;
            }
            int rlen;
            for (;;) { // periodic timeout - check abort request flag
                rlen = esp_http_client_read(mClient, buf, bufSize);
                if (rlen <= 0) {
                    if (errno == ETIMEDOUT) {
                        return;
                    }
                    ESP_LOGW(TAG, "Error receiving http stream, errno: %d, rxBytes: %llu, rlen = %d",
                        errno, mBytesTotal, rlen);
                }
                break;
            }
            if (rlen > 0) {
                mRingBuf.commitWrite(rlen);
                ESP_LOGI(TAG, "Received %d bytes, wrote to ringbuf (%d)", rlen, mRingBuf.totalDataAvail());
                //TODO: Implement IceCast metadata support
                if (mWaitingPrefill && mRingBuf.totalDataAvail() >= mPrefillAmount) {
                    setWaitingPrefill(false);
                }
                return;
            }
            // even though len == 0 means graceful disconnect, i.e.
            //track end => should go to next track, this often happens when
            // network lags and stream sender aborts sending to us
            // => we should reconnect.
            ESP_LOGW(TAG, "Reconnecting and retrying...");
            destroyClient(); // just in case
            connect(true);
        }
        // network retry gave up
        if (!nextTrack()) {
            setState(kStatePaused);
            sendEvent(kEventNoMoreTracks, nullptr, 0);
            return;
        }
        // Try next track
        mBytePos = 0;
        sendEvent(kEventNewTrack, mUrl, 0);
        destroyClient(); // just in case
        connect();
    }
}

void HttpNode::send()
{
/*
    int wrlen = esp_http_client_write(mClient, buf.data, buf.size);
    if (wrlen <= 0) {
        ESP_LOGE(TAG, "Failed to write data to http stream, wrlen=%d, errno=%d", wrlen, errno);
    }
    return wrlen;
*/
}
void HttpNode::setUrl(const char* url)
{
    if (!mTaskId) {
        doSetUrl(url);
    } else {
        ESP_LOGI(mTag, "Posting setUrl command");
        mCmdQueue.post(kCommandSetUrl, strdup(url));
    }
}

bool HttpNode::dispatchCommand(Command &cmd)
{
    if (AudioNodeWithTask::dispatchCommand(cmd)) {
        return true;
    }
    switch(cmd.opcode) {
    case kCommandSetUrl:
        destroyClient();
        doSetUrl((const char*)cmd.arg);
        free(cmd.arg);
        cmd.arg = nullptr;
        mRingBuf.clear();
        mFlushRequested = true; // request flush along the pipeline
        setWaitingPrefill(true);
        setState(kStateRunning);
        break;
    default: return false;
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
        if (mIsWriter) {
            while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
                send();
            }
        } else {
            while (!mTerminate && (mCmdQueue.numMessages() == 0)) {
                recv(); // retries and goes to new playlist track
            }
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
}

HttpNode::HttpNode(size_t bufSize)
: AudioNodeWithTask("http-node", kStackSize), mRingBuf(bufSize),
  mPrefillAmount(bufSize * 3 / 4)
{
}

AudioNode::StreamError HttpNode::pullData(DataPullReq& dp, int timeout)
{
    ElapsedTimer tim;
    if (mFlushRequested) {
        mFlushRequested = false;
        return kStreamFlush;
    }
    while (mWaitingPrefill) {
        auto ret = waitPrefillChange(timeout);
        if (ret < 0) {
            return kStreamStopped;
        } else if (ret == 0) {
            return kTimeout;
        }
    }
    timeout -= tim.msElapsed();
    if (timeout <= 0) {
        return kTimeout;
    }
    if (!dp.size) { // caller only wants to get the stream format
        auto ret = mRingBuf.waitForData(timeout);
        if (ret < 0) {
            return kStreamStopped;
        } else if (ret == 0) {
            return kTimeout;
        }
        dp.fmt = mStreamFormat;
        return kNoError;
    }
    tim.reset();
    auto ret = mRingBuf.contigRead(dp.buf, dp.size, timeout);
    if (tim.msElapsed() > timeout) {
        ESP_LOGW(mTag, "RingBuf read took more than timeout: took %d, timeout %d", tim.msElapsed(), timeout);
    }
    if (ret < 0) {
        return kStreamStopped;
    } else if (ret == 0){
        return kTimeout;
    } else {
        dp.size = ret;
        return kNoError;
    }
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

int8_t HttpNode::waitPrefillChange(int msTimeout)
{
    auto bits = mEvents.waitForOneAndReset(kEvtPrefillChange|kEvtStopRequest, msTimeout);
    if (bits & kEvtStopRequest) {
        return -1;
    } else if (bits & kEvtPrefillChange) {
        return 1;
    } else {
        return 0;
    }
}
