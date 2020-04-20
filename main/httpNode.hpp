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
#include <audio_type_def.h>
#include <strings.h>
#include "ringbuf.hpp"
#include "queue.hpp"
#include "utils.hpp"
#include "audioNode.hpp"
#include "playlist.hpp"

class HttpNode: public AudioNodeWithTask
{
protected:
    enum { kPollTimeoutMs = 1000, kStreamBufferSize = 512, kStackSize = 3 * 1024 };
    enum: uint16_t {
        kHttpEventType = kUserEventTypeBase << 1,
        kEventOnRequest = 1 | kHttpEventType,
        kEventNewTrack = 2 | kHttpEventType,
        kEventNoMoreTracks = 3 | kHttpEventType
    };
    enum: uint8_t { kCommandPause, kCommandRun, kCommandSetUrl };
    char* mUrl = nullptr;
    StreamFormat mStreamFormat;
    esp_http_client_handle_t mClient = nullptr;
    bool mAutoNextTrack = true; /* connect next track without open/close */
    Playlist mPlaylist; /* media playlist */
    size_t mStackSize;
    RingBuf mRingBuf;
    int64_t mBytesTotal;
    EventGroup mEvents;
    int mRecvSize = 2048;
    static esp_err_t httpHeaderHandler(esp_http_client_event_t *evt);
    static esp_codec_type_t codecFromContentType(const char* content_type);
    bool sendEvent(uint16_t type, void* buffer, int bufSize);
    bool isPlaylist();
    bool createClient();
    bool parseContentType();
    bool parseResponseAsPlaylist();
    void doSetUrl(const char* url);
    bool connect(bool isReconnect=false);
    void disconnect();
    void destroyClient();
    bool nextTrack();
    void recv();
    void send();
    void nodeThreadFunc();
    virtual bool dispatchCommand(Command &cmd);
public:
    HttpNode(const char* tag, size_t bufSize);
    virtual ~HttpNode();
    virtual int pullData(char* buf, size_t size, int timeout, StreamFormat& fmt);
    void setUrl(const char* url);
};
