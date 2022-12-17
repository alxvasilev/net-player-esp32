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
void EqualizerNode::setFormat(StreamFormat fmt)
{
    mFormat = fmt; // so that we can detect the next change
    auto sr = fmt.sampleRate();
    if (fmt.bitsPerSample() != 16) {
        ESP_LOGW(mTag, "Only 16 bits per sample supported, but stream is %d-bit. Disabling equalizer", fmt.bitsPerSample());
        mBypass = true;
    }
    else if (sr != 44100 && sr != 48000 && sr != 22050 && sr != 11025) {
        ESP_LOGE(mTag, "Unsupported samplerate of %d Hz", sr);
        mBypass = true;
    } else {
        mBypass = false;
        equalizerReinit(fmt);
    }
}

AudioNode::StreamError EqualizerNode::pullData(DataPullReq &dpr)
{
    auto event = mPrev->pullData(dpr);
    if (event) {
        return event;
    }
    MutexLocker locker(mMutex);
    if (dpr.fmt != mFormat) {
        setFormat(dpr.fmt);
    }
    if (volLevelEnabled()) {
        volumeNotifyLevelCallback(); // notify previous levels to compensate output buffering delay
        volumeGetLevel(dpr);
    }
    if (volProcessingEnabled()) {
        volumeProcess(dpr);
    }
    if (!mBypass) {
        esp_equalizer_process(mEqualizer, (unsigned char*)dpr.buf, dpr.size, mSampleRate, mChanCount);
    }
    return kNoError;
}
