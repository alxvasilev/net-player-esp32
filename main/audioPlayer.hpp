#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"

class AudioPlayer
{
public:
    enum InputType: uint8_t
    { kInputNone = 0, kInputHttp = 1, kInputA2dp };
    enum OutputType: uint8_t
    { kOutputNone = 0, kOutputI2s, kOutputA2dp };
    enum CodecType: uint8_t
    { kCodecNone = 0, kCodecMp3, kCodecAac };
    enum PlayerState: uint8_t
    { kStateStopped = 0, kStatePlaying, kStatePaused };
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2 };

    esp_periph_set_handle_t mPeriphSet;
    InputType mInputType = kInputNone;
    OutputType mOutputType = kOutputNone;
    CodecType mDecoderType = kCodecNone;
    PlayerState mState = kStateStopped;
    Flags mFlags;
    audio_element_handle_t mStreamIn = nullptr;
    audio_element_handle_t mDecoder = nullptr;
    audio_element_handle_t mEqualizer = nullptr;
    audio_element_handle_t mStreamOut = nullptr;
    audio_element_handle_t mSamplerateSource = nullptr;
    audio_pipeline_handle_t mPipeline = nullptr;
    static const int mEqualizerDefaultGainTable[];

    static int httpEventHandler(http_stream_event_msg_t *msg);
    static esp_err_t inputFormatEventCb(audio_element_handle_t el,
        audio_event_iface_msg_t *event, void *ctx);
    static const char* codecTypeToStr(CodecType type);

    void createInputHttp();
    void createInputA2dp();

    void createOutputI2s();
    void createOutputA2dp();
    void createOutputElement(OutputType type);
    void createDecoderByType(CodecType type);

    void createEqualizer();
    void destroyEqualizer();
//==
    void createOutputSide(OutputType outType);
    void destroyOutputSide();

    void createInputSide(InputType inType, CodecType codecType);
    void destroyInputSide();

    void linkPipeline();
public:
    static constexpr const char* const TAG = "AudioPlayer";
    void setLogLevel(esp_log_level_t level) { esp_log_level_set(TAG, level); }
    AudioPlayer(OutputType outType, bool useEq=true);
    ~AudioPlayer();
    void setSourceUrl(const char* url, CodecType codecType);
    PlayerState state() const { return mState; }
    void play();
    void pause();
    void resume();
    void stop();
    int getVolume();
    bool setVolume(int vol);
    int changeVolume(int step);
};

#endif
