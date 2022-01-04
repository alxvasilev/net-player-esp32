#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#include "equalizer.hpp"
#include "audioNode.hpp"
#include "volume.hpp"

class EqualizerNode: public AudioNode, public DefaultVolumeImpl
{
public:
    enum: uint8_t { kBandCount = 10 };
    static const uint16_t bandFreqs[kBandCount];
protected:
    Mutex mMutex;
    StreamFormat mFormat;
    int mSampleRate = 0; // cached from mFormat, for performance
    uint8_t mChanCount = 0; // cached from mFormat, for performance
    void* mEqualizer = nullptr;
    float mGains[kBandCount];
    bool mGetAudioLevelBeforeEq = false;
    void equalizerReinit(StreamFormat fmt);
    void updateBandGain(uint8_t band);
public:
    EqualizerNode(const float* gains=nullptr);
    virtual Type type() const { return kTypeEqualizer; }
    virtual StreamError pullData(DataPullReq &dpr, int timeout) override;
    virtual void confirmRead(int size) override { mPrev->confirmRead(size); }
    void setBandGain(uint8_t band, float dbGain);
    void setAllGains(const float* gains);
    void zeroAllGains();
    float bandGain(uint8_t band);
    const float* allGains() { return mGains; }
    void monitorLevelAtInput(bool enable) { mGetAudioLevelBeforeEq = enable; }
    virtual IAudioVolume* volumeInterface() override { return this; }
};

#endif // EQUALIZERNODE_HPP
