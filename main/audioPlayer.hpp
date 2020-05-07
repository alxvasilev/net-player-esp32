#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include "audioNode.hpp"
//#include <audio_type_def.h>
#include "utils.hpp"
class DecoderNode;
class EqualizerNode;

class AudioPlayer
{
public:
    static constexpr int kHttpBufSize = 40 * 1024;
protected:
    enum Flags: uint8_t
    { kFlagUseEqualizer = 1, kFlagListenerHooked = 2, kFlagNoWaitPrefill = 4 };

    esp_periph_set_handle_t mPeriphSet;
    Flags mFlags;
    std::unique_ptr<AudioNodeWithTask> mStreamIn;
    std::unique_ptr<DecoderNode> mDecoder;
    std::unique_ptr<EqualizerNode> mEqualizer;
    std::unique_ptr<AudioNodeWithTask> mStreamOut;
    IAudioVolume* mVolumeInterface = nullptr;
    static const int mEqualizerDefaultGainTable[];

    void createInputA2dp();
    void createOutputA2dp();
//==
    void createPipeline(AudioNode::Type inType, AudioNode::Type outType);
    void destroyPipeline();
    void detectVolumeNode();
public:
    static constexpr const char* const TAG = "AudioPlayer";
    static const uint16_t equalizerFreqs[10];
    Mutex mutex;
    void setLogLevel(esp_log_level_t level) { esp_log_level_set(TAG, level); }
    AudioPlayer(AudioNode::Type inType, AudioNode::Type outType, bool useEq=true);
    ~AudioPlayer();
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
    const float *equalizerDumpGains();
    bool equalizerSetBand(int band, int8_t dbGain);
    // format is: bandIdx1=gain1;bandIdx2=gain2....
    bool equalizerSetGainsBulk(char* str, size_t len);
};

#endif
