#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
#include "utils.hpp"
#include "nvsHandle.hpp"
#include "eventGroup.hpp"
#include <st7735.hpp>
#include "stationList.hpp"
#include "recorder.hpp"
#include "vuDisplay.hpp"
#include "dlna.hpp"

class DecoderNode;
class EqualizerNode;
class ST7735Display;
namespace http { class Server; }
namespace nvs { class NVSHandle; }
struct TrackInfo;

class AudioPlayer: public IAudioPipeline
{
public:
    enum: uint32_t {
        kHttpBufSizeInternal = 35 * 1024,
        kHttpBufSizeSpiRam = 800 * 1024,
        kHttpBufPrefillSpiRam = 65536,
        kDefTitleScrollFps = 15,
        kLcdNetSpeedUpdateIntervalUs = 1 * 1000000
    };
    enum PlayerMode: uint8_t {
        kModeInvalid = 0,
        kModeFlagHttp = 0x80,
        kModeRadio = kModeFlagHttp,
        kModeDlna = kModeFlagHttp | 1,
        kModeUrl = kModeFlagHttp | 2,
        kModeBluetoothSink = AudioNode::kTypeA2dpIn,
        kModeSpdifIn = AudioNode::kTypeI2sIn,
        //====
        kModeDefault = kModeRadio
    };
    static const char* playerModeToStr(PlayerMode mode);
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };
    enum: uint8_t
    { kEventTerminating = 1, kEventScroll = 2, kEventVolLevel = 4, kEventTerminated = 8 };
    enum {
        kI2sStackSize = 9000, kI2sCpuCore = 1,
        kLcdTaskStackSize = 2200, kLcdTaskPrio = 10, kLcdTaskCore = 0
    };
    typedef enum: char {
        kSymBlank = ' ',
        kSymStopped = 38,
        kSymPaused = 37,
        kSymConnecting = 34,
        kSymPlaying = 36,
        kSymFavorite = 33,
        kSymRecording = 34,
        kSymRecEnabled = 35
    } GuiPlayState; // used for displaying pause/connecting icon
    static constexpr const Font& kPictoFont = Font_7x11;
    enum { kEqGainPrecisionDiv = 2 };
    static const float sDefaultEqGains[];
    Flags mFlags;
    std::unique_ptr<AudioNodeWithState> mStreamIn;
    std::unique_ptr<DecoderNode> mDecoder;
    std::unique_ptr<EqualizerNode> mEqualizer;
    std::unique_ptr<AudioNodeWithTask> mStreamOut;
    IAudioVolume* mVolumeInterface = nullptr;
    IAudioVolume* mVuLevelInterface = nullptr;
    NvsHandle mNvsHandle;
    ST7735Display& mLcd;
    EventGroup mEvents;
    http::Server& mHttpServer;
    std::unique_ptr<DlnaHandler> mDlna;
    unique_ptr_mfree<TrackInfo> mTrackInfo;
    uint32_t mStreamSeqNo = 0;
    PlayerMode mPlayerMode;
// general display stuff
    ST7735Display::Color mFontColor = ST7735Display::rgb(255, 255, 128);
    VuDisplay mVuDisplay;
// track name scroll stuff
    DynBuffer mLcdTrackTitle;
    int16_t mTitleScrollCharOffset = 0;
    int8_t mTitleScrollPixOffset = 0;
    bool mTitleScrollEnabled = false;
    int16_t mLastShownNetSpeed = -1;
    static void audioLevelCb(void* ctx);
//====
    static void lcdTimedDrawTask(void* ctx);

    void createInputA2dp();
    void createOutputA2dp();
//==
    bool createPipeline(AudioNode::Type inType, AudioNode::Type outType);
    void destroyPipeline();
    void detectVolumeNode();
    std::string printPipeline();
    void loadSettings();
    void init(PlayerMode mode, AudioNode::Type outType);
    PlayerMode initFromNvs();
    float equalizerDoSetBandGain(int band, float dbGain);
    void equalizerSaveGains();
    void createDlnaHandler();
    void setPlayerMode(PlayerMode mode);
    void pipelineStop();
    void lcdInit();
    void lcdDrawGui();
    void initTimedDrawTask();
    void lcdUpdatePlayState(char state);
    void lcdSetupForTrackTitle();
    void lcdUpdateTrackTitle(const char* buf);
    void lcdScrollTrackTitle(int step=1);
    void lcdUpdateArtistName(const char* name);
    void lcdUpdateStationInfo();
    void lcdUpdateRecIcon();
    // stream info line
    void lcdWriteStreamInfo(int8_t charOfs, const char* str);
    void lcdUpdateCodec(CodecType codec);
    void lcdUpdateAudioFormat(StreamFormat fmt);
    void lcdUpdateNetSpeed();
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
    void registerHttpGetHandler(const char* path, esp_err_t(*handler)(httpd_req_t*));
public:
    Mutex mutex;
    PlayerMode mode() const { return mPlayerMode; }
    std::unique_ptr<StationList> stationList;
    const TrackInfo* trackInfo() const { return mTrackInfo.get(); }
    void setLogLevel(esp_log_level_t level);
    AudioPlayer(PlayerMode mode, AudioNode::Type outType, ST7735Display& lcd, http::Server& httpServer, bool useEq=true);
    AudioPlayer(ST7735Display& lcd, http::Server& httpServer);
    ~AudioPlayer();
    AudioNode::Type inputType() const { return mStreamIn->type(); }
    AudioNode::Type outputType() const { return mStreamOut->type(); }
    NvsHandle& nvs() { return mNvsHandle; }
    void changeInput(PlayerMode playerMode);
    bool playUrl(const char* url, PlayerMode playerMode, const char* record=nullptr);
    bool playUrl(TrackInfo* trackInfo, PlayerMode playerMode, const char* record=nullptr);
    esp_err_t playStation(const char* id);
    bool isStopped() const;
    bool isPaused() const;
    bool isPlaying() const;
    void play();
    void pause();
    void resume();
    void stop();
    uint32_t positionTenthSec() const;
    static bool playerModeIsValid(PlayerMode mode);
    int volumeGet();
    bool volumeSet(uint16_t vol);
    uint16_t volumeChange(int step);
    const float *equalizerGains(); // requies player lock while accessing the gains array
    bool equalizerSetBand(int band, float dbGain);
    // format is: bandIdx1=gain1;bandIdx2=gain2....
    bool equalizerSetGainsBulk(char* str, size_t len);
    void registerUrlHanlers();
    // AudioNode::EventHandler interface
    virtual void onNodeEvent(AudioNode& node, uint32_t type, uintptr_t arg, size_t numArg) override;
    virtual void onNodeError(AudioNode& node, int error) override;
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
