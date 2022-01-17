#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
#include "utils.hpp"
#include "nvsHandle.hpp"
#include "eventGroup.hpp"
#include <st7735.hpp>
#include "stationList.hpp"
#include "recorder.hpp"
#include <limits>
#include "volume.hpp"

class DecoderNode;
class EqualizerNode;
class ST7735Display;

namespace nvs {
    class NVSHandle;
}
class VuDisplay {
    enum {
        kLevelSmoothFactor = 4, kDefPeakHoldMs = 30, kDefPeakDropSpeed = 20,
        kDefLedWidth = 1, kDefLedHeight = 10, kDefLedSpacing = 1, kDefChanSpacing = 2
    };
    enum: uint16_t { kLevelMax = std::numeric_limits<int16_t>::max() };
    struct ChanCtx
    {
        int16_t barY;
        int16_t avgLevel = 0;
        int16_t peakLevel = 0;
        int16_t prevBarLen = 0;
        uint8_t peakTimer = 0;
    };
    ST7735Display& mLcd;
    ChanCtx mLeftCtx;
    ChanCtx mRightCtx;
    int8_t mStepWidth; // mLedWidth + led spacing
    int8_t mLedWidth;
    int8_t mLedHeight;
    int8_t mChanSpacing;
    int16_t mYellowStartX;
    int32_t mLevelPerLed;
    uint8_t mPeakDropTicks;
    uint8_t mPeakHoldTicks;
    inline uint16_t ledColor(int16_t ledX, int16_t level);
    void calculateLevels(ChanCtx& ctx, int16_t level);
    void drawChannel(ChanCtx& ctx, int16_t level);
    inline int16_t numLedsForLevel(int16_t level);
public:
    VuDisplay(ST7735Display& lcd): mLcd(lcd) {}
    void init(NvsHandle& nvs);
    void update(const IAudioVolume::StereoLevels& levels);
};

class AudioPlayer: public AudioNode::EventHandler
{
public:
    static constexpr int kHttpBufSizeInternal = 35 * 1024;
    static constexpr int kHttpBufSizeSpiRam = 350 * 1024;
    static constexpr int kTitleScrollFps = 30;
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };
    enum: uint8_t
    { kEventTerminating = 1, kEventScroll = 2, kEventVolLevel = 4, kEventTerminated = 8 };
    enum { kLcdTaskStackSize = 1200, kLcdTaskPrio = 10 };
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
    VuDisplay mVuDisplay;
// track name scroll stuff
    bool mTitleScrollEnabled = false;
    DynBuffer mTrackTitle;
    int16_t mTitleScrollCharOffset = 0;
    int8_t mTitleScrollPixOffset = 0;

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
    static esp_err_t nvsGetParamUrlHandler(httpd_req_t* req);
    static esp_err_t nvsSetParamUrlHandler(httpd_req_t* req);
    void registerHttpGetHandler(httpd_handle_t server,
        const char* path, esp_err_t(*handler)(httpd_req_t*));
public:
    Mutex mutex;
    std::unique_ptr<StationList> stationList;
    void setLogLevel(esp_log_level_t level);
    AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, ST7735Display& lcd, bool useEq=true);
    AudioPlayer(ST7735Display& lcd);
    ~AudioPlayer();
    AudioNode::Type inputType() const { return mStreamIn->type(); }
    AudioNode::Type outputType() const { return mStreamOut->type(); }
    NvsHandle& nvs() { return mNvsHandle; }
    void changeInput(AudioNode::Type inType);
    bool playUrl(const char* url, const char* record=nullptr);
    esp_err_t playStation(const char* id);
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
