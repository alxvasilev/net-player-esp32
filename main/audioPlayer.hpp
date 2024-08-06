#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
#include "utils.hpp"
#include "nvsHandle.hpp"
#include "eventGroup.hpp"
#include <lcdColor.hpp>
#include <font.hpp>
#include "stationList.hpp"
#include "recorder.hpp"
#include "vuDisplay.hpp"
#include "dlna.hpp"
#include <framebuf.hpp>
#include <gfx.hpp>
#include <nvsSimple.hpp>

class DecoderNode;
class EqualizerNode;
class ST7735Display;
class MDns;
typedef Color565 LcdColor;
namespace http { class Server; }
namespace nvs { class NVSHandle; }
struct TrackInfo;

extern NvsSimple nvsSimple;
extern MDns mdns;
extern Font font_CamingoBold43;
extern Font font_Camingo22;
extern Font font_Camingo32;
extern Font font_Icons22;

class AudioPlayer: public IAudioPipeline
{
public:
    static constexpr const char* kDefaultMdnsName = "netplayer";
    static const char* mdnsName();
    enum: uint32_t {
        kHttpBufSizeInternal = 35 * 1024,
        kHttpBufSizeSpiRam = 800 * 1024,
        kHttpBufPrefillSpiRam = 65536,
        kDefTitleScrollFps = 25,
        kLcdNetSpeedUpdateIntervalUs = 1 * 1000000
    };
    enum PlayerMode: uint8_t {
        kModeInvalid = 0,
        kModeRadio = AudioNode::kTypeHttpIn,
        kModeDlna = AudioNode::kTypeHttpIn | AudioNode::kTypeFlagPlayerCtrl | 1,
        kModeUrl = AudioNode::kTypeHttpIn | 2,
        kModeBluetoothSink = AudioNode::kTypeA2dpIn,
        kModeSpotify = AudioNode::kTypeSpotify,
        kModeSpdifIn = AudioNode::kTypeI2sIn,
        //====
        kModeDefault = kModeRadio
    };
    static const char* playerModeToStr(PlayerMode mode);
protected:
    enum: uint8_t
    { kEventTerminating = 1, kEventScroll = 2, kEventVolLevel = 4, kEventTerminated = 8 };
    enum {
        kI2sStackSize = 4096, kI2sCpuCore = 0,
        kI2sDmaBufCnt = 4, // 1 buffer is 1024 samples
        kLcdTaskStackSize = 2200, kLcdTaskPrio = 20, kLcdTaskCore = 1,
        kMaxTrackTitleLen = 100
    };
    enum {
        kLcdArtistNameLineY = 38, kLcdPlayStateLineY = 76, kLcdTrackTitleY = 106
    };

    enum {
        kSymBlank = 32,
        kSymFavorite = 33,
        kSymRecording = 34
    };
    static constexpr const Font& kPictoFont = font_Icons22;
    enum { kDefaultVolume = 15 };
    static const float sDefaultEqGains[];
    std::unique_ptr<AudioNodeWithState> mStreamIn;
    std::unique_ptr<DecoderNode> mDecoder;
    std::unique_ptr<EqualizerNode> mEqualizer;
    std::unique_ptr<AudioNodeWithTask> mStreamOut;
    IAudioVolume* mVolumeInterface = nullptr;
    IAudioVolume* mVuLevelInterface = nullptr;
    int64_t mTsLastVuLevel = 0;
    bool mStopping = false; // set while stopping the pipeline, to ignore error signalled by nodes during the process
    NvsHandle mNvsHandle;
    ST7735Display& mLcd;
    LcdFrameBuf mDmaFrameBuf;
    LcdFrameBuf mTitleTextFrameBuf;
    EventGroup mEvents;
    http::Server& mHttpServer;
    std::unique_ptr<DlnaHandler> mDlna;
    unique_ptr_mfree<TrackInfo> mTrackInfo;
    PlayerMode mPlayerMode;
    int mBufLowThreshold = 0;
    uint16_t mBufLowDisplayGradient = 0;
    int16_t mMuteVolume = -1;
    uint8_t mVolumeCap;
// general display stuff
    Color565 mFontColor = Color565(255, 255, 128);
    VuDisplay mVuDisplay;
    IAudioVolume::StereoLevels mVuLevels = {0,0};
// track name scroll stuff
    DynBuffer mLcdTrackTitle;
    int16_t mTitleTextWidth = -1;
    int16_t mTitleScrollPixOffset = 0;
    bool mTitleScrollEnabled = false;
    int8_t mTitleScrollStep = 1;
    uint32_t mLastShownNetSpeed = 0xffffffff;
    uint8_t mLatchedBufUnderrunState = 0xff;
    uint8_t mDisplayedBufUnderrunTimer = 0;
    StreamFormat mStreamFormat;
    static void audioLevelCb(void* ctx);
//====
    static void lcdTimedDrawTaskFunc(void* ctx);
    void lcdTimedDrawTask();

    void createInputA2dp();
    void createOutputA2dp();
//==
    bool createPipeline(AudioNode::Type inType, AudioNode::Type outType);
    void pipelineStop();
    void destroyPipeline();
    void detectVolumeNode();
    std::string printPipeline();
    IPlayerCtrl* playController() { return mStreamIn ? mStreamIn->playerCtrl() : nullptr; }
    void loadSettings();
    void init(PlayerMode mode, AudioNode::Type outType);
    PlayerMode initFromNvs();
    void createDlnaHandler();
    void setPlayerMode(PlayerMode mode);
    void onNewStream(StreamFormat fmt, int sourceBps);
    bool streamIsCpuHeavy() const;
    // GUI stuff
    void lcdInit();
    void lcdDrawGui();
    void initTimedDrawTask();
    void lcdUpdatePlayState(const char* text, bool isRecording=false);
    void lcdBlitTrackTitle();
    void lcdUpdateTrackTitle(const char* buf);
    void lcdScrollTrackTitle();
    void lcdUpdateArtistName(const char* name);
    void lcdUpdateStationInfo();
    void lcdUpdateTrackDisplay();
    // stream info line
    void lcdWriteStreamInfo(int8_t charOfs, const char* str);
    void lcdUpdateCodec();
    void lcdUpdateAudioFormat();
    // net speed stuff
    void lcdUpdateNetSpeed();
    void lcdRenderNetSpeed(uint32_t speed, uint32_t bufDataSize);
    void lcdResetNetSpeedIndication();
    void lcdShowBufUnderrunImmediate();
    // web URL handlers
    static esp_err_t playUrlHandler(httpd_req_t *req);
    static esp_err_t pauseUrlHandler(httpd_req_t *req);
    static esp_err_t volumeUrlHandler(httpd_req_t *req);
    static esp_err_t equalizerSetUrlHandler(httpd_req_t *req);
    static esp_err_t equalizerDumpUrlHandler(httpd_req_t *req);
    static esp_err_t getStatusUrlHandler(httpd_req_t *req);
    static esp_err_t resetSubsystemUrlHandler(httpd_req_t *req);
    static esp_err_t nvsGetParamUrlHandler(httpd_req_t* req);
    static esp_err_t nvsSetParamUrlHandler(httpd_req_t* req);
    static esp_err_t changeInputUrlHandler(httpd_req_t *req);
    static AudioNode::Type playerModeToInNodeType(PlayerMode mode);
    void registerHttpGetHandler(const char* path, esp_err_t(*handler)(httpd_req_t*));
    bool doPlayUrl(const char* url, PlayerMode playerMode, const char* record=nullptr);
public:
    Mutex mutex;
    http::Server& httpServer() const { return mHttpServer; }
    PlayerMode mode() const { return mPlayerMode; }
    std::unique_ptr<StationList> stationList;
    const TrackInfo* trackInfo() const { return mTrackInfo.get(); }
    void setLogLevel(esp_log_level_t level);
    AudioPlayer(ST7735Display& lcd, http::Server& httpServer, PlayerMode mode=kModeInvalid,
        AudioNode::Type outType=AudioNode::kTypeUnknown);
    ~AudioPlayer();
    AudioNode::Type inputType() const { return mStreamIn->type(); }
    AudioNode::Type outputType() const { return mStreamOut->type(); }
    NvsHandle& nvs() { return mNvsHandle; }
    void switchMode(PlayerMode playerMode, bool persist=false);
    bool playUrl(const char* url, PlayerMode playerMode, const char* record=nullptr);
    bool playUrl(TrackInfo* trackInfo, PlayerMode playerMode, const char* record=nullptr);
    std::string url() const;
    esp_err_t playStation(const char* id);
    bool isStopped() const;
    bool isPaused() const;
    bool isPlaying() const;
    void play();
    void pause();
    void resume();
    void stop();
    void stop(const char* caption);
    uint32_t positionTenthSec() const;
    static bool playerModeIsValid(PlayerMode mode);
    int volumeGet();
    uint8_t volumeSet(uint8_t vol);
    int volumeChange(int step);
    bool isMuted() const { return mMuteVolume >= 0; }
    void mute();
    void unmute();
    const int8_t* equalizerGains(); // requies player lock while accessing the gains array
    // format is: bandIdx1=gain1;bandIdx2=gain2....
    bool equalizerSetGainsFromString(char* str, size_t len);
    void registerUrlHanlers();
    // AudioNode::EventHandler interface
    virtual bool onNodeEvent(AudioNode& node, uint32_t type, size_t numArg, uintptr_t arg) override;
    virtual void onNodeError(AudioNode& node, int error, uintptr_t arg) override;
};

struct TrackInfo {
    const char* url;
    const char* trackName;
    const char* artistName;
    uint32_t durationMs;
    static TrackInfo* Create(const char* aUrl, const char* trkName, int tnLen, const char* artName, int anLen, uint32_t durMs)
    {
        auto urlLen = strlen(aUrl) + 1;
        auto inst = (TrackInfo*)malloc(sizeof(TrackInfo) + urlLen + tnLen + anLen + 3);
        inst->durationMs = durMs;
        inst->url = (char*)inst + sizeof(TrackInfo);
        memcpy((char*)inst->url, aUrl, urlLen);
        auto next = inst->url + urlLen;
        if (trkName) {
            inst->trackName = next;
            next += tnLen + 1;
            memcpy((char*)inst->trackName, trkName, tnLen);
            (char&)inst->trackName[tnLen] = 0;
        } else {
            inst->trackName = nullptr;
        }
        if (artName) {
            inst->artistName = next;
            //next += anLen + 1;
            memcpy((char*)inst->artistName, artName, anLen);
            (char&)inst->artistName[anLen] = 0;
        } else {
            inst->artistName = nullptr;
        }
        return inst;
    }
    static TrackInfo* Create(const char *aUrl, const char *trkName, const char *artName, uint32_t durMs)
    {
        return Create(aUrl, trkName, trkName ? strlen(trkName) : 0, artName, artName ? strlen(artName) : 0, durMs);
    }
};

#endif
