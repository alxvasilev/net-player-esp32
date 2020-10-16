#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
#include "utils.hpp"
#include "nvsHandle.hpp"
#include "eventGroup.hpp"
#include <st7735.hpp>

class DecoderNode;
class EqualizerNode;
class ST7735Display;

namespace nvs {
    class NVSHandle;
}

class AudioPlayer: public AudioNode::EventHandler
{
public:
    static constexpr int kHttpBufSize = 20 * 1024;
    static constexpr int kTitleScrollTickPeriodMs = 50;
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };
    enum: uint8_t
    { kEventTerminating = 1, kEventScroll = 2, kEventVolLevel = 4, kEventTerminated = 8 };
    enum { kVuLedWidth = 8, kVuLedHeight = 4, kVuLevelSmoothFactor = 4,
           kVuPeakHoldTime = 30, kVuPeakDropTime = 2
    };
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
// track name scroll stuff
    CbTimer mTitleScrollTimer;
    DynBuffer mTrackTitle;
    int16_t mTitleScrollCharOffset = 0;
    int8_t mTitleScrollPixOffset = 0;
// Volume level indicator stuff
    int16_t mLevelPerVuLed;
    int16_t mVuYellowStartX;
    int16_t mVuRedStartX;
    int16_t mVuLeftAvg = 0;
    int16_t mVuRightAvg = 0;
    int16_t mVuPeakLeft = 0;
    uint8_t mVuPeakTimerLeft = 0;
    int16_t mVuPeakRight = 0;
    uint8_t mVuPeakTimerRight = 0;
    static void audioLevelCb(void* ctx);
    inline uint16_t vuLedColor(int16_t ledX, int16_t level);
    void lcdUpdateVolLevel();
//====
    static void titleSrollTickCb(void* ctx);
    static void lcdTimedDrawTask(void* ctx);

    void createInputA2dp();
    void createOutputA2dp();
//==
    bool createPipeline(AudioNode::Type inType, AudioNode::Type outType);
    void destroyPipeline();
    void detectVolumeNode();
    std::string printPipeline();
    void loadSettings();
    void initFromNvs();
    float equalizerDoSetBandGain(int band, float dbGain);
    void equalizerSaveGains();
    void lcdInit();
    void initTimedDrawTask();
    void lcdUpdateModeInfo();
    void lcdUpdatePlayState();
    void lcdSetupForTrackTitle();
    void lcdUpdateTrackTitle(const char* buf, int size);
    void lcdScrollTrackTitle();
    void lcdUpdateStationInfo();
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
    Playlist playlist;
    void setLogLevel(esp_log_level_t level) { esp_log_level_set(TAG, level); }
    AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, ST7735Display& lcd, bool useEq=true);
    AudioPlayer(ST7735Display& lcd);
    ~AudioPlayer();
    AudioNode::Type inputType() const { return mStreamIn->type(); }
    AudioNode::Type outputType() const { return mStreamOut->type(); }
    NvsHandle& nvs() { return mNvsHandle; }
    void changeInput(AudioNode::Type inType);
    void playUrl(const char* url);
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
    virtual bool onEvent(AudioNode *self, uint32_t type, void *buf, size_t bufSize) override;
};

#endif
