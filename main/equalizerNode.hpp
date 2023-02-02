#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#define BQ_DEBUG
#include "equalizer.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
public:
    enum: uint8_t { kBandCount = 10 };
protected:
    Mutex mMutex;
    bool mBypass = false;
    StreamFormat mFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    uint8_t mChanCount = 0; // cached from mFormat, for performance
    Equalizer<kBandCount, float> mEqualizerLeft;
    Equalizer<kBandCount, float> mEqualizerRight;
    float mGains[kBandCount];
    void (EqualizerNode::*mProcessFunc)(AudioNode::DataPullReq& req) = nullptr;
    void process16bitStereo(AudioNode::DataPullReq&);
    void process32bitStereo(AudioNode::DataPullReq&);
    void equalizerReinit(StreamFormat fmt);
    void setFormat(StreamFormat fmt);
    void updateBandGain(uint8_t band);
public:
    EqualizerNode(IAudioPipeline& parent, const float* gains=nullptr);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamError pullData(DataPullReq &dpr) override;
    virtual void confirmRead(int size) override { mPrev->confirmRead(size); }
    void disable(bool disabled) { mBypass = disabled; }
    void setBandGain(uint8_t band, float dbGain);
    void setAllGains(const float* gains);
    void zeroAllGains();
    float bandGain(uint8_t band);
    const float* allGains() { return mGains; }
    const EqBandConfig& bandCfg(int n) const { return mEqualizerLeft.bandConfig(n); }
    virtual IAudioVolume* volumeInterface() override { return this; }
};

#endif // EQUALIZERNODE_HPP
