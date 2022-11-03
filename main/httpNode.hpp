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
    char* mRecordingStationName = nullptr;
    StreamFormat mStreamFormat;
    esp_http_client_handle_t mClient = nullptr;
    bool mAutoNextTrack = true; /* connect next track without open/close */
    Playlist mPlaylist; /* media playlist */
    size_t mStackSize;
    RingBuf mRingBuf;
    volatile bool mWaitingPrefill = true;
    volatile bool mFlushRequested = false;
    int mPrefillAmount;
    int mContentLen;
    IcyParser mIcyParser;
    int mIcyEventAfterBytes = -1;
    bool mIcyHadNewTrack = false;
    std::unique_ptr<TrackRecorder> mRecorder;
    int mIoTimeoutMs = -1;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static CodecType codecFromContentType(const char* content_type);
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    bool parseResponseAsPlaylist();
    void doSetUrl(const char* url, const char* staName);
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    bool nextTrack();
    bool recv();
    void setWaitingPrefill(bool prefill);
    int8_t waitPrefillChange(int msTimeout);
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
    virtual StreamError pullData(DataPullReq &dp, int timeout);
    virtual void confirmRead(int size);
    virtual void pause(bool wait);
    void setIoTimeout(int timeoutMs) { mIoTimeoutMs = timeoutMs; }
    void setUrl(const char* url, const char* recStationName);
    bool isConnected() const;
    const char* trackName() const;
    bool recordingIsActive() const;
    bool recordingIsEnabled() const;
};
