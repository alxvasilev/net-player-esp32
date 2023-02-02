#include "equalizerNode.hpp"
#include <esp_equalizer.h>

EqualizerNode::EqualizerNode(IAudioPipeline& parent, const float *gains)
: AudioNode(parent, "equalizer"),
  mEqualizerLeft(EqBandConfig::kPresetTenBand), mEqualizerRight(EqBandConfig::kPresetTenBand)
{
    if (gains) {
        memcpy(mGains, gains, sizeof(mGains));
    } else {
        memset(mGains, 0, sizeof(mGains));
    }
}

void EqualizerNode::updateBandGain(uint8_t band)
{
    float gain = mGains[band];
    mEqualizerLeft.setBandGain(band, gain);
    mEqualizerRight.setBandGain(band, gain);
}

void EqualizerNode::equalizerReinit(StreamFormat fmt)
{
    ESP_LOGI(mTag, "Equalizer reinit");
    mFormat = fmt;
    mChanCount = fmt.numChannels();
    mSampleRate = fmt.sampleRate();
    mEqualizerLeft.init(mSampleRate, mGains);
    mEqualizerRight.init(mSampleRate, mGains);
    auto bps = fmt.bitsPerSample();
    if (bps == 16) {
        mProcessFunc = &EqualizerNode::process16bitStereo;
    } else if (bps == 24 || bps == 32) {
        mProcessFunc = &EqualizerNode::process32bitStereo;
    } else {
        ESP_LOGW(mTag, "Unsupported bits per sample %d", bps);
    }
}

void EqualizerNode::setBandGain(uint8_t band, float dbGain)
{
    MutexLocker locker(mMutex);
    mGains[band] = dbGain;
    updateBandGain(band);
}

void EqualizerNode::setAllGains(const float* gains)
{
    MutexLocker locker(mMutex);
    memcpy(mGains, gains, sizeof(mGains));
    for (int i = 0; i < kBandCount; i++) {
        updateBandGain(i);
    }
}
void EqualizerNode::zeroAllGains()
{
    MutexLocker locker(mMutex);
    memset(mGains, 0, sizeof(mGains));
    for (int i = 0; i < kBandCount; i++) {
        updateBandGain(i);
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
    equalizerReinit(fmt);
}
void EqualizerNode::process16bitStereo(DataPullReq& dpr) {
    auto end = (int16_t*)(dpr.buf + dpr.size);
    for (auto sample = (int16_t*)dpr.buf; sample < end;) {
        *sample = mEqualizerLeft.processAndNarrow(*sample);
        sample++;
        *sample = mEqualizerRight.processAndNarrow(*sample);
        sample++;
    }
}
void EqualizerNode::process32bitStereo(DataPullReq& dpr) {
    auto end = (int32_t*)(dpr.buf + dpr.size);
    for (auto sample = (int32_t*)dpr.buf; sample < end;) {
        *sample = mEqualizerLeft.process(*sample);
        sample++;
        *sample = mEqualizerRight.process(*sample);
        sample++;
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
        (this->*mProcessFunc)(dpr);
    }
    return kNoError;
}
