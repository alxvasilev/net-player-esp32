#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
#include "utils.hpp"
#include "nvsHandle.hpp"
#include "eventGroup.hpp"
#include <st7735.hpp>
#include "stationList.hpp"
#include "recorder.hpp"

class DecoderNode;
class EqualizerNode;
class ST7735Display;

namespace nvs {
    class NVSHandle;
}

class AudioPlayer: public AudioNode::EventHandler
{
public:
    static constexpr int kHttpBufSizeInternal = 35 * 1024;
    static constexpr int kHttpBufSizeSpiRam = 350 * 1024;
    static constexpr int kTitleScrollTickPeriodMs = 40;
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };
    enum: uint8_t
    { kEventTerminating = 1, kEventScroll = 2, kEventVolLevel = 4, kEventTerminated = 8 };
    enum { kLcdTaskStackSize = 1200, kLcdTaskPrio = 10,
           kVuLevelSmoothFactor = 4, kVuPeakHoldTime = 30, kVuPeakDropTime = 2,
           kVuLedWidth = 20, kVuLedHeight = 8, kVuLedSpacing = 3
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
    NvsHandle mNvsHandle;
    ST7735Display& mLcd;
    EventGroup mEvents;
// general display stuff
    ST7735Display::Color mFontColor = ST7735Display::rgb(255, 255, 128);
// track name scroll stuff
    bool mTitleScrollEnabled = false;
    DynBuffer mTrackTitle;
    int16_t mTitleScrollCharOffset = 0;
    int8_t mTitleScrollPixOffset = 0;
// Volume level indicator stuff
    int16_t mLevelPerVuLed;
    int16_t mVuYellowStartX;
    int16_t mVuRedStartX;
    struct VuLevelCtx
    {
        int16_t barY;
        int16_t avgLevel = 0;
        int16_t peakLevel = 0;
        uint8_t peakTimer = 0;
    };
    VuLevelCtx mVuLeftCtx;
    VuLevelCtx mVuRightCtx;

    static void audioLevelCb(void* ctx);
    inline uint16_t vuLedColor(int16_t ledX, int16_t level);
    void lcdUpdateVolLevel();
    void vuCalculateLevels(VuLevelCtx& ctx, int16_t level);
    void vuDrawChannel(VuLevelCtx& ctx, int16_t level);

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
    void init(AudioNode::Type inType, AudioNode::Type outType);
    void initFromNvs();
    float equalizerDoSetBandGain(int band, float dbGain);
    void equalizerSaveGains();
    void lcdInit();
    void lcdDrawGui();
    void initTimedDrawTask();
    void lcdUpdatePlayState(char state);
    void lcdSetupForTrackTitle();
    void lcdUpdateTrackTitle(const char* buf);
    void lcdScrollTrackTitle(int step=1);
    void lcdUpdateStationInfo();
    void lcdUpdateRecIcon();
    // web URL handlers
    static esp_err_t playUrlHandler(httpd_req_t *req);
    static esp_err_t pauseUrlHandler(httpd_req_t *req);
    static esp_err_t volumeUrlHandler(httpd_req_t *req);
    static esp_err_t equalizerSetUrlHandler(httpd_req_t *req);
    static esp_err_t equalizerDumpUrlHandler(httpd_req_t *req);
    static esp_err_t getStatusUrlHandler(httpd_req_t *req);
    void registerHttpGetHandler(httpd_handle_t server,
        const char* path, esp_err_t(*handler)(httpd_req_t*));
public:
    static constexpr const char* const TAG = "AudioPlayer";
    Mutex mutex;
    std::unique_ptr<StationList> stationList;
    void setLogLevel(esp_log_level_t level) { esp_log_level_set(TAG, level); }
    AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, ST7735Display& lcd, bool useEq=true);
    AudioPlayer(ST7735Display& lcd);
    ~AudioPlayer();
    AudioNode::Type inputType() const { return mStreamIn->type(); }
    AudioNode::Type outputType() const { return mStreamOut->type(); }
    NvsHandle& nvs() { return mNvsHandle; }
    void changeInput(AudioNode::Type inType);
    bool playUrl(const char* url, const char* record=nullptr);
    bool playStation(bool next);
    bool isStopped() const;
    bool isPaused() const;
    bool isPlaying() const;
    void play();
    void pause();
    void resume();
    void stop();
    int volumeGet();
    bool volumeSet(uint16_t vol);
    uint16_t volumeChange(int step);
    const float *equalizerGains(); // requies player lock while accessing the gains array
    bool equalizerSetBand(int band, float dbGain);
    // format is: bandIdx1=gain1;bandIdx2=gain2....
    bool equalizerSetGainsBulk(char* str, size_t len);
    void registerUrlHanlers(httpd_handle_t server);
    // AudioNode::EventHandler interface
    virtual bool onEvent(AudioNode *self, uint32_t type, uintptr_t arg, size_t bufSize) override;
};

#endif
