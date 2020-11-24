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
#include "recorder.hpp"

class HttpNode: public AudioNodeWithTask
{
protected:
    enum { kPollTimeoutMs = 1000, kHttpClientBufSize = 512, kReadSize = 1024,
           kStackSize = 3600 };
    enum: uint8_t { kCommandSetUrl = AudioNodeWithTask::kCommandLast + 1,
                    kCommandNotifyFlushed };
    // Read mode dictates how the pullData() caller behaves. Since it may
    // need to wait for the read mode to change to a specific value, the enum values
    // are flags
    enum: uint8_t { kEvtPrefillChange = kEvtLast << 1 };
    char* mUrl = nullptr;
    StreamFormat mStreamFormat;
    esp_http_client_handle_t mClient = nullptr;
    bool mAutoNextTrack = true; /* connect next track without open/close */
    Playlist mPlaylist; /* media playlist */
    size_t mStackSize;
    RingBuf mRingBuf;
    volatile bool mWaitingPrefill = true;
    volatile bool mFlushRequested = false;
    int mPrefillAmount;
    uint32_t mContentLen;
    int32_t mIcyCtr = 0;
    int32_t mIcyInterval = 0;
    int16_t mIcyRemaining = 0;
    void clearAllIcyInfo();
    std::unique_ptr<TrackRecorder> mRecorder;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static CodecType codecFromContentType(const char* content_type);
    bool isPlaylist();
    int icyProcessRecvData(char* buf, int len);
    void icyParseMetaData();
    bool createClient();
    bool parseContentType();
    bool parseResponseAsPlaylist();
    void doSetUrl(const char* url);
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    bool nextTrack();
    void recv();
    void setWaitingPrefill(bool prefill);
    int8_t waitPrefillChange(int msTimeout);
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd);
    virtual void doStop();
public:
    enum: uint32_t {
        kEventConnecting = kEventLastGeneric << 1,
        kEventConnected = kEventLastGeneric << 2,
        kEventNextTrack = kEventLastGeneric << 3,
        kEventNoMoreTracks = kEventLastGeneric << 4,
        kEventTrackInfo = kEventLastGeneric << 5
    };
    class IcyInfo
    {
    protected:
        friend class HttpNode;
        DynBuffer mIcyMetaBuf;
        BufPtr<char> mStaName = nullptr;
        BufPtr<char> mStaDesc = nullptr;
        BufPtr<char> mStaGenre = nullptr;
        BufPtr<char> mStaUrl = nullptr;
        void clear();
    public:
        Mutex mutex;
        const char* trackName() const {
            return mIcyMetaBuf.dataSize() ? mIcyMetaBuf.buf() : nullptr;
        }
        const char* staName() const { return mStaName.ptr(); }
        const char* staDesc() const { return mStaDesc.ptr(); }
        const char* staGenre() const { return mStaGenre.ptr(); }
        const char* staUrl() const { return mStaUrl.ptr(); }
    };
    IcyInfo icyInfo;
    HttpNode(size_t bufSize);
    virtual ~HttpNode();
    virtual Type type() const { return kTypeHttpIn; }
    virtual StreamError pullData(DataPullReq &dp, int timeout);
    virtual void confirmRead(int size);
    void setUrl(const char* url);
    bool isConnected() const;
    const char* trackName() const;
    void startRecording(const char* stationName, TrackRecorder::IEventHandler* handler = nullptr);
    bool isRecordingNow() const { return mRecorder && mRecorder->isRecording(); }
    bool isRecordingEnabled() const { return mRecorder.get() != nullptr; }
};
