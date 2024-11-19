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
#include "speedProbe.hpp"
#include "recorder.hpp"
#include "streamRingQueue.hpp"

class HttpNode: public AudioNodeWithTask, public IInputAudioNode
{
public:
    class UrlInfo;
    mutable Mutex mMutex;
protected:
    enum {
        kHttpRecvTimeoutMs = 10000, kHttpClientBufSize = 1024, kRingQueueLen = 256, kStackSize = 5120
    };
    enum: uint8_t { kCommandSetUrl = AudioNodeWithTask::kCommandLast + 1 };
    // Read mode dictates how the pullData() caller behaves. Since it may
    // need to wait for the read mode to change to a specific value, the enum values
    // are flags
    std::unique_ptr<UrlInfo> mUrlInfo;
    esp_http_client_handle_t mClient = nullptr;
    StreamFormat mInFormat; // is not Codec, because PCM needs sample format info as well
    StreamId mOutStreamId = 0;
    Playlist mPlaylist; /* media playlist */
    StreamRingQueue<kRingQueueLen> mRingBuf;
    int64_t mStreamByteCtr = 0;
    int mContentLen = 0;
    unique_ptr_mfree<const char> mStationNameHdr;
    std::string mLastTitle;
    IcyParser mIcyParser;
    std::unique_ptr<TrackRecorder> mRecorder;
    int16_t mRxChunkSize = 0;
    bool mAcceptsRangeRequests = false;
    volatile int mWaitingPrefill = 0;
    volatile bool mPrefillSentFirstData = false;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    void onHttpHeader(const char* key, const char* val);
    bool canResume() const { return (mContentLen != 0) && mAcceptsRangeRequests; }
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    int8_t handleResponseAsPlaylist(int32_t contentLen);
    void doSetUrl(UrlInfo* urlInfo);
    void updateUrl(const char* url);
    void clearRingBuffer();
    void prefillStart();
    void prefillComplete();
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
    IcyInfo& icyInfo() { return mIcyParser; }
    HttpNode(IAudioPipeline& parent);
    virtual ~HttpNode();
    virtual Type type() const { return kTypeHttpIn; }
    virtual StreamEvent pullData(PacketResult& pr);
    virtual void onStopped() override { recordingCancelCurrent(); }
    virtual DataPacket* peekData(bool& preceded) override;
    virtual StreamPacket* peek() override;
    virtual void notifyFormatDetails(StreamFormat fmt) override;
    virtual IInputAudioNode* inputNodeIntf() override { return static_cast<IInputAudioNode*>(this); }
    void setUrlAndStart(UrlInfo* urlInfo);
    bool isConnected() const;
    const char* trackName() const;
    bool recordingIsActive() const;
    bool recordingIsEnabled() const;
    virtual uint32_t pollSpeed() override;
    virtual uint32_t bufferedDataSize() const override { return mRingBuf.dataSize(); }
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
    mutable LinkSpeedProbe mSpeedProbe;
    bool mIsBufUnderrun = false;
    void setUnderrunState(bool isUnderrun);
public:
    const char* url() const { return mUrlInfo ? mUrlInfo->url : nullptr; }
    const char* recStaName() const { return mUrlInfo ? mUrlInfo->recStaName : nullptr; }
};
