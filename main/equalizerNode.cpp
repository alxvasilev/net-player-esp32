#include "equalizerNode.hpp"
#include <esp_equalizer.h>

const uint16_t EqualizerNode::bandFreqs[kBandCount] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

EqualizerNode::EqualizerNode(IAudioPipeline& parent, const float *gains)
: AudioNode(parent, "equalizer")
{
    if (gains) {
        memcpy(mGains, gains, sizeof(mGains));
    } else {
        memset(mGains, 0, sizeof(mGains));
    }
}

void EqualizerNode::updateBandGain(uint8_t band)
{
    float  gain = mGains[band];
    esp_equalizer_set_band_value(mEqualizer, gain, band, 0);
    esp_equalizer_set_band_value(mEqualizer, gain, band, 1);
}

void EqualizerNode::equalizerReinit(StreamFormat fmt)
{
    ESP_LOGI(mTag, "Equalizer reinit");
    mFormat = fmt;
    mChanCount = fmt.numChannels();
    mSampleRate = fmt.sampleRate();
    if (mEqualizer) {
        esp_equalizer_uninit(mEqualizer);
    }
    mEqualizer = esp_equalizer_init(mChanCount, mSampleRate, kBandCount, 0);
    for (int i = 0; i < kBandCount; i++) {
        updateBandGain(i);
    }
}

void EqualizerNode::setBandGain(uint8_t band, float dbGain)
{
    MutexLocker locker(mMutex);
    mGains[band] = dbGain;
    if (mEqualizer) {
        updateBandGain(band);
    }
}

void EqualizerNode::setAllGains(const float* gains)
{
    MutexLocker locker(mMutex);
    memcpy(mGains, gains, sizeof(mGains));
    if (mEqualizer) {
        for (int i = 0; i < kBandCount; i++) {
            updateBandGain(i);
        }
    }
}
void EqualizerNode::zeroAllGains()
{
    MutexLocker locker(mMutex);
    memset(mGains, 0, sizeof(mGains));
    if (mEqualizer) {
        for (int i = 0; i < kBandCount; i++) {
            updateBandGain(i);
        }
    }
}

float EqualizerNode::bandGain(uint8_t band)
{
    MutexLocker locker(mMutex);
    return mGains[band];
}
AudioNode::StreamError EqualizerNode::pullData(DataPullReq &dpr)
{
    MutexLocker locker(mMutex);
    auto ret = mPrev->pullData(dpr);
    if (ret) {
        return ret;
    }
    if (dpr.fmt != mFormat) {
        if (dpr.fmt.bitsPerSample() != 16) {
            ESP_LOGE(mTag, "Only 16 bits per sample supported, but stream is %d-bit", dpr.fmt.bitsPerSample());
            return kErrStreamFmt;
        }
        equalizerReinit(dpr.fmt);
    }
    if (mGetAudioLevelBeforeEq) {
        getAudioLevel(dpr);
        processVolume(dpr);
        esp_equalizer_process(mEqualizer, (unsigned char*)dpr.buf, dpr.size, mSampleRate, mChanCount);
    } else {
        processVolume(dpr);
        esp_equalizer_process(mEqualizer, (unsigned char*)dpr.buf, dpr.size, mSampleRate, mChanCount);
        getAudioLevel(dpr);
    }
    return kNoError;
}
