#include <sys/unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include "esp_log.h"
#include "errno.h"
#include "esp_system.h"
#include <esp_http_client.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "audioNode.hpp"
#include "playlist.hpp"
#include "icyParser.hpp"
#include "recorder.hpp"
#include "staticQueue.hpp"

class HttpNode: public AudioNodeWithTask
{
public:
    struct UrlInfo {
        uint32_t streamId;
        const char* url;
        const char* recStaName;
        static UrlInfo* Create(const char* aUrl, uint32_t streamId, const char* aRecStaName) noexcept
        {
            auto urlLen = strlen(aUrl) + 1;
            auto staLen = aRecStaName ? strlen(aRecStaName) : 0;
            auto inst = (UrlInfo*)malloc(sizeof(UrlInfo) + urlLen + staLen);
            inst->streamId = streamId;
            inst->url = (char*)inst + sizeof(UrlInfo);
            memcpy((char*)inst->url, aUrl, urlLen);
            if (aRecStaName) {
                inst->recStaName = inst->url + urlLen;
                memcpy((char*)inst->recStaName, aRecStaName, staLen);
            } else {
                inst->recStaName = nullptr;
            }
            return inst;
        }
    };
protected:
    enum { kHttpRecvTimeoutMs = 2000, kHttpClientBufSize = 512, kReadSize = 1024, kStackSize = 3600 };
    enum: uint8_t { kCommandSetUrl = AudioNodeWithTask::kCommandLast + 1, kCommandNotifyFlushed };
    // Read mode dictates how the pullData() caller behaves. Since it may
    // need to wait for the read mode to change to a specific value, the enum values
    // are flags
    enum: uint8_t { kEvtPrefillChange = kEvtLast << 1 };
    struct QueuedStreamEvent {
        int64_t streamPos;
        union {
            struct {
                uint32_t streamId: 24;
                CodecType codec;
            };
            void* data;
        };
        StreamError type;
        QueuedStreamEvent(uint32_t aStreamPos, StreamError aType, CodecType aCodec, uint32_t aStreamId):
            streamPos(aStreamPos), streamId(aStreamId), codec(aCodec), type(aType) {}
        QueuedStreamEvent(uint32_t aStreamPos, StreamError aType, void* aData):
            streamPos(aStreamPos), data(aData), type(aType) {}
        ~QueuedStreamEvent() {
            if (type == kTitleChanged) {
                free(data);
            }
        }
    };
    UrlInfo* mUrlInfo = nullptr;
    esp_http_client_handle_t mClient = nullptr;
    CodecType mInCodec = kCodecUnknown;
    CodecType mOutCodec = kCodecUnknown;
    Playlist mPlaylist; /* media playlist */
    RingBuf mRingBuf;
    StaticQueue<QueuedStreamEvent, 6> mStreamEventQueue;
    int64_t mRxByteCtr = 0;
    int64_t mStreamStartPos = 0;
    volatile bool mWaitingPrefill = true;
    int mPrefillAmount;
    int mContentLen;
    IcyParser mIcyParser;
    std::unique_ptr<TrackRecorder> mRecorder;
    const char* url() const { return mUrlInfo ? mUrlInfo->url : nullptr; }
    const char* recStaName() const { return mUrlInfo ? mUrlInfo->recStaName : nullptr; }
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static CodecType codecFromContentType(const char* content_type);
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    int8_t handleResponseAsPlaylist(int32_t contentLen);
    void doSetUrl(UrlInfo* urlInfo);
    void updateUrl(const char* url);
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    bool recv();
    template <typename... Args>
    bool postStreamEvent_Lock(int64_t streamPos, StreamError event, Args... args);
    template <typename... Args>
    bool postStreamEvent_NoLock(int64_t streamPos, StreamError event, Args... args);
    void setWaitingPrefill(bool prefill);
    bool waitPrefillChange();
    int delayFromRetryCnt(int tries);
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd);
    virtual void doStop();
// recording stuff
    bool recordingMaybeEnable();
    void recordingStop();
    void recordingCancelCurrent();
public:
    enum: uint32_t {
        kEventConnecting = kEventLastGeneric << 1,
        kEventConnected = kEventLastGeneric << 2,
        kEventNextTrack = kEventLastGeneric << 3,
        kEventNoMoreTracks = kEventLastGeneric << 4,
        kEventTrackInfo = kEventLastGeneric << 5,
        kEventRecording = kEventLastGeneric << 6
    };
    mutable Mutex mMutex;
    IcyInfo& icyInfo() { return mIcyParser; }
    HttpNode(IAudioPipeline& parent, size_t bufSize, size_t prefillAmount);
    virtual ~HttpNode();
    virtual Type type() const { return kTypeHttpIn; }
    virtual StreamError pullData(DataPullReq &dp);
    virtual void confirmRead(int size);
    virtual void pause(bool wait);
    void setUrl(UrlInfo* urlInfo);
    bool isConnected() const;
    const char* trackName() const;
    bool recordingIsActive() const;
    bool recordingIsEnabled() const;
};
template <typename... Args>
bool HttpNode::postStreamEvent_Lock(int64_t streamPos, StreamError event, Args... args) {
    for(;;) {
        bool ok;
        {
            MutexLocker locker(mMutex);
            ok = mStreamEventQueue.emplaceBack(streamPos, event, args...);
        }
        if (ok) {
            return true;
        }
        if (mRingBuf.waitForReadOp(-1) < 0) {
            return false;
        }
    }
}
template <typename... Args>
bool HttpNode::postStreamEvent_NoLock(int64_t streamPos, StreamError event, Args... args) {
    while (!mStreamEventQueue.emplaceBack(streamPos, event, args...)) {
        MutexUnlocker unlock(mMutex);
        if (mRingBuf.waitForReadOp(-1) < 0) {
            return false;
        }
    }
    return true;
}
