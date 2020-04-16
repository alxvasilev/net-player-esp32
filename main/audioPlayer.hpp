#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <audio_element.h>
#include <audio_pipeline.h>
#include <audio_event_iface.h>
#include <audio_type_def.h>
#include "utils.hpp"

class AudioPlayer
{
public:
    enum InputType: uint8_t
    { kInputNone = 0, kInputHttp = 1, kInputA2dp };
    enum OutputType: uint8_t
    { kOutputNone = 0, kOutputI2s, kOutputA2dp };
    enum PlayerState: uint8_t
    { kStateStopped = 0, kStatePlaying, kStatePaused };
    static constexpr int kHttpBufSize = 40 * 1024;
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };

    esp_periph_set_handle_t mPeriphSet;
    InputType mInputType = kInputNone;
    OutputType mOutputType = kOutputNone;
    esp_codec_type_t mDecoderType = ESP_CODEC_TYPE_UNKNOW;
    PlayerState mState = kStateStopped;
    Flags mFlags;
    audio_element_handle_t mStreamIn = nullptr;
    audio_element_handle_t mDecoder = nullptr;
    audio_element_handle_t mEqualizer = nullptr;
    audio_element_handle_t mStreamOut = nullptr;
    audio_element_handle_t mSamplerateSource = nullptr;
    audio_pipeline_handle_t mPipeline = nullptr;
    static const int mEqualizerDefaultGainTable[];

    static int httpStreamEventHandler(http_stream_event_msg_t *msg);
    static esp_err_t httpElementEventHandler(audio_element_handle_t el,
        audio_event_iface_msg_t *event, void *ctx);

    static esp_err_t inputFormatEventCb(audio_element_handle_t el,
        audio_event_iface_msg_t *event, void *ctx);

    static esp_err_t outputEventCb(audio_element_handle_t el,
        audio_event_iface_msg_t* msg, void* ctx);

    static const char* codecTypeToStr(esp_codec_type_t type);

    void createInputHttp();
    void createInputA2dp();
    void createOutputI2s();
    void createOutputA2dp();
    void createOutputElement(OutputType type);
    void createEqualizer();
    void createDecoderByType(esp_codec_type_t type);
    void destroyDecoder();
    void changeDecoder(esp_codec_type_t type);
//==

    void createPipeline(InputType inType, esp_codec_type_t codecType, OutputType outType);
    void destroyPipeline();

    void registerAllAndLinkPipeline();
public:
    static constexpr const char* const TAG = "AudioPlayer";
    static const uint16_t equalizerFreqs[10];
    Mutex mutex;
    void setLogLevel(esp_log_level_t level) { esp_log_level_set(TAG, level); }
    AudioPlayer(InputType inType, esp_codec_type_t codecType,
                OutputType outType, bool useEq=true);
    ~AudioPlayer();
    void playUrl(const char* url, esp_codec_type_t codecType=ESP_CODEC_TYPE_UNKNOW);
    PlayerState state() const { return mState; }
    void play();
    void pause();
    void resume();
    void stop();
    int volumeGet();
    bool volumeSet(int vol);
    int volumeChange(int step);
    int* equalizerDumpGains();
    bool equalizerSetBand(int band, int level);
    // format is: bandIdx1=gain1;bandIdx2=gain2....
    bool equalizerSetGainsBulk(char* str, size_t len);
};

#endif
