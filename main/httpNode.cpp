#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <audio_type_def.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "httpNode.hpp"

static const char *TAG = "HTTP_NODE";

esp_codec_type_t HttpNode::codecFromContentType(const char* content_type)
{
    if (strcasecmp(content_type, "mp3") == 0 ||
        strcasecmp(content_type, "audio/mp3") == 0 ||
        strcasecmp(content_type, "audio/mpeg") == 0 ||
        strcasecmp(content_type, "binary/octet-stream") == 0 ||
        strcasecmp(content_type, "application/octet-stream") == 0) {
        return ESP_CODEC_TYPE_MP3;
    }
    if (strcasecmp(content_type, "audio/aac") == 0 ||
        strcasecmp(content_type, "audio/x-aac") == 0 ||
        strcasecmp(content_type, "audio/mp4") == 0 ||
        strcasecmp(content_type, "audio/aacp") == 0 ||
        strcasecmp(content_type, "video/MP2T") == 0) {
        return ESP_CODEC_TYPE_AAC;
    }
    if (strcasecmp(content_type, "application/ogg") == 0) {
        return ESP_CODEC_TYPE_OGG;
    }
    if (strcasecmp(content_type, "audio/wav") == 0) {
        return ESP_CODEC_TYPE_WAV;
    }
    if (strcasecmp(content_type, "audio/opus") == 0) {
        return ESP_CODEC_TYPE_OPUS;
    }
    if (strcasecmp(content_type, "application/vnd.apple.mpegurl") == 0 ||
        strcasecmp(content_type, "vnd.apple.mpegURL") == 0) {
        return ESP_AUDIO_TYPE_M3U8;
    }
    if (strncasecmp(content_type, "audio/x-scpls", strlen("audio/x-scpls")) == 0) {
        return ESP_AUDIO_TYPE_PLS;
    }
    return ESP_CODEC_TYPE_UNKNOW;
}

bool HttpNode::sendEvent(uint16_t type, void* buf, int bufSize)
{
    if (!mEventHandler || ((type & mSubscribedEvents) == 0)) {
        return true;
    }
    auto ret = mEventHandler->onEvent(this, type, buf, bufSize);
    if (!ret) {
        ESP_LOGW(TAG, "User event handler returned false for event %d", type);
    }
    return ret;
}

bool HttpNode::isPlaylist()
{
    auto codec = mStreamFormat.mCodec;
    if (codec == ESP_AUDIO_TYPE_M3U8 || codec == ESP_AUDIO_TYPE_PLS) {
        return true;
    }
    char *dot = strrchr(mUrl, '.');
    return (dot && ((strcasecmp(dot, ".m3u") == 0 || strcasecmp(dot, ".m3u8") == 0)));
}

void HttpNode::doSetUrl(const char *url)
{
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
    cfg.user_data = this;
    cfg.timeout_ms = kPollTimeoutMs;
    cfg.buffer_size = kStreamBufferSize;
    cfg.method = mIsWriter ? HTTP_METHOD_POST : HTTP_METHOD_GET;

    mClient = esp_http_client_init(&cfg);
    if (!mClient)
    {
        ESP_LOGE(TAG, "Error creating http client, probably out of memory");
        return false;
    };
    return true;
}
bool HttpNode::parseContentType()
{
    char* contentType = nullptr;
    if (esp_http_client_get_header(mClient, "Content-Type", &contentType) != ESP_OK) {
        mStreamFormat.mCodec = ESP_CODEC_TYPE_UNKNOW;
        return false;
    }
    ESP_LOGD(TAG, "Received content-type:  %s", contentType);
    mStreamFormat.mCodec = codecFromContentType(contentType);
    return true;
}

bool HttpNode::connect(bool isReconnect)
{
    myassert(mState != kStateStopped);
    if (!mUrl) {
        ESP_LOGE(TAG, "open: URI has not been set");
        return false;
    }

    ESP_LOGD(TAG, "Opening URI '%s'", mUrl);
    // if not initialize http client, initial it
    if (!mClient) {
        if (!createClient()) {
            return false;
        }
    }

    if (!isReconnect) {
        mBytePos = 0;
    }
    if (mBytePos) { // we are resuming, send position
        char rang_header[32];
        snprintf(rang_header, 32, "bytes=%lld-", mBytePos);
        esp_http_client_set_header(mClient, "Range", rang_header);
    }
    if (!sendEvent(kEventOnRequest, mClient, 0)) {
        return false;
    }

    if (mIsWriter) {
        return esp_http_client_open(mClient, -1); // -1 for content length means chunked encoding
    }
    for (int tries = 0; tries < 4; tries++) {
        auto err = esp_http_client_open(mClient, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open http stream, error %s", esp_err_to_name(err));
            return false;
        }
        /*
         * Due to the total byte of content has been changed after seek, set info.total_bytes at beginning only.
         */
        int64_t contentLen = esp_http_client_fetch_headers(mClient);
        if (!mBytePos) {
            mBytesTotal = contentLen;
        }

        ESP_LOGI(TAG, "Content-length reported by server: %d", (int)mBytesTotal);
        int status_code = esp_http_client_get_status_code(mClient);
        if (status_code == 301 || status_code == 302) {
            esp_http_client_set_redirection(mClient);
            continue;
        }
        else if (status_code != 200 && status_code != 206) {
            ESP_LOGE(TAG, "Invalid HTTP stream, status code = %d", status_code);
            return false;
        }
        parseContentType();
        if (parseResponseAsPlaylist()) {
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
            auto bufSize = mRingBuf.getWriteBuf(buf);
            if (bufSize < 0) { // command queued
                return;
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
                //TODO: Implement IceCast metadata support
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
        setUrl(mUrl);
        setState(kStateRunning);
        break;
    default: return false;
    }
    return true;
}

void HttpNode::nodeThreadFunc()
{
    ESP_LOGD(TAG, "Task started");
    for (;;) {
        processMessages();
        if (mTerminate) {
            return;
        }
        myassert(mState == kStateRunning);
        if (connect() != ESP_OK) {
            setState(kStatePaused);
            continue;
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

HttpNode::~HttpNode()
{
    stop();
    destroyClient();
}

HttpNode::HttpNode(const char* tag, size_t bufSize)
: AudioNodeWithTask(tag, kStackSize), mRingBuf(bufSize)
{
}
int HttpNode::pullData(char* buf, size_t size, int timeout, StreamFormat& fmt)
{
    fmt = mStreamFormat;
    return mRingBuf.read(buf, size, timeout);
}
