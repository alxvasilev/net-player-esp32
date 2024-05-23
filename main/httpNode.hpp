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
#include "utils.hpp"
#include "audioNode.hpp"
#include "playlist.hpp"
#include "icyParser.hpp"
#include "recorder.hpp"
#include "streamRingQueue.hpp"

class HttpNode: public AudioNodeWithTask
{
public:
    class UrlInfo;
protected:
    enum {
        kHttpRecvTimeoutMs = 10000, kHttpClientBufSize = 512, kRingQueueLen = 256, kStackSize = 5120
    };
    enum: uint8_t { kCommandSetUrl = AudioNodeWithTask::kCommandLast + 1 };
    // Read mode dictates how the pullData() caller behaves. Since it may
    // need to wait for the read mode to change to a specific value, the enum values
    // are flags
    std::unique_ptr<UrlInfo> mUrlInfo;
    esp_http_client_handle_t mClient = nullptr;
    StreamFormat mInFormat; // is not Codec, because PCM needs sample format info as well
    uint32_t mOutStreamId = 0;
    Playlist mPlaylist; /* media playlist */
    StreamRingQueue<kRingQueueLen> mRingBuf;
    int64_t mStreamByteCtr = 0;
    int mContentLen = 0;
    IcyParser mIcyParser;
    std::unique_ptr<TrackRecorder> mRecorder;
    int16_t mNetRecvSize = 0;
    bool mAcceptsRangeRequests = false;
    volatile int mWaitingPrefill = 0;
    volatile bool mPrefillSentFirstData = false;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static StreamFormat parseLpcmContentType(const char* ctype, int bps);
    static StreamFormat codecFromContentType(const char* content_type);
    void onHttpHeader(const char* key, const char* val);
    bool canResume() const { return (mContentLen != 0) && mAcceptsRangeRequests; }
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    int8_t handleResponseAsPlaylist(int32_t contentLen);
    void doSetUrl(UrlInfo* urlInfo);
    void updateUrl(const char* url);
    void clearRingBuffer();
    uint32_t currentStreamId() const;
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    int8_t recv();
    int delayFromRetryCnt(int tries);
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd);
    virtual void onStopRequest() override;
// recording stuff
    bool recordingMaybeEnable();
    void recordingStop();
    void recordingCancelCurrent();
public:
    mutable Mutex mMutex;
    IcyInfo& icyInfo() { return mIcyParser; }
    HttpNode(IAudioPipeline& parent);
    virtual ~HttpNode();
    virtual Type type() const { return kTypeHttpIn; }
    virtual StreamEvent pullData(PacketResult& pr);
    virtual void onStopped() override { recordingCancelCurrent(); }
    virtual DataPacket* peekData(bool& preceded) override;
    virtual StreamPacket* peek() override;
    virtual void streamFormatDetails(StreamFormat fmt) override;
    void setUrlAndStart(UrlInfo* urlInfo);
    bool isConnected() const;
    void setWaitingPrefill(int amout); // locking required
    const char* trackName() const;
    bool recordingIsActive() const;
    bool recordingIsEnabled() const;
    uint32_t pollSpeed() const;
    int32_t bufferedDataSize() const { return mRingBuf.dataSize(); }
    void logStartOfRingBuf(const char* msg);
    struct UrlInfo {
        uint32_t streamId;
        const char* url;
        const char* recStaName;
        static UrlInfo* Create(const char* aUrl, uint32_t streamId, const char* aRecStaName) noexcept
        {
            auto urlLen = strlen(aUrl) + 1;
            auto staLen = aRecStaName ? strlen(aRecStaName) + 1 : 0;
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
    class LinkSpeedProbe {
        ElapsedTimer mTimer;
        uint32_t mBytes = 0;
        uint32_t mAvgSpeed = 0;
    public:
        uint32_t average() const { return mAvgSpeed; }
        void onTraffic(uint32_t nBytes) { mBytes += nBytes; }
        void reset() { mBytes = 0; mAvgSpeed = 0; mTimer.reset(); }
        uint32_t poll() {
            int64_t elapsed = mTimer.usElapsed();
            mTimer.reset();
            if (elapsed == 0) {
                elapsed = 1;
            }
            uint32_t speed = ((int64_t)mBytes * 1000000 + (elapsed >> 1)) / elapsed; //rounded int division
            mBytes = 0;
            mAvgSpeed = (mAvgSpeed * 3 + speed + 2) >> 2; // rounded int division by 4
            return speed;
        }
    };
    mutable LinkSpeedProbe mSpeedProbe;
    bool mIsBufUnderrun = false;
    void setUnderrunState(bool isUnderrun);
public:
    const char* url() const { return mUrlInfo ? mUrlInfo->url : nullptr; }
    const char* recStaName() const { return mUrlInfo ? mUrlInfo->recStaName : nullptr; }
};
