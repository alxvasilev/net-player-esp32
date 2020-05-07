#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#include "equalizer.hpp"
#include "audioNode.hpp"

class EqualizerNode: public AudioNode, public IAudioVolume
{
protected:
    Mutex mMutex;
    double mGainMult = 1.0;
    int mSampleRate = 0;
    Equalizer mEqualizerLeft;
    Equalizer mEqualizerRight;
public:
    EqualizerNode(): AudioNode("eq"){}
    virtual Type type() const { return kTypeEqualizer; }
    virtual uint8_t flags() const { return kSupportsVolume; }
    virtual StreamError pullData(DataPullReq &dpr, int timeout) override;
    virtual void confirmRead(int size) override { mPrev->confirmRead(size); }
protected:
    template <typename Sample>
    void process(char* buf, int size, bool stereo)
    {
        auto end = (Sample*)(buf + size);
        if (stereo) {
            for (Sample* pSample = (Sample*)(buf); pSample < end;) {
                *pSample = mEqualizerLeft.process(*pSample) * mGainMult;
                pSample++;
                *pSample = mEqualizerRight.process(*pSample) * mGainMult;
                pSample++;
            }
        } else {
            for (Sample* pSample = (Sample*)(buf); pSample < end; pSample++) {
                *pSample = mEqualizerLeft.process(*pSample) * mGainMult;
            }
        }
    }
public:
    void setBandGain(uint8_t band, double dbGain) {
        MutexLocker locker(mMutex);
        mEqualizerLeft.setBandGain(band, dbGain);
        mEqualizerRight.setBandGain(band, dbGain);
    }
    double bandGain(uint8_t band) {
        MutexLocker locker(mMutex);
        return mEqualizerLeft.filter(band).dbGain();
    }
    // IAudioVolume interface
    uint16_t getVolume() const { return mGainMult * 100; }
    void setVolume(uint16_t vol) { mGainMult = (double)vol / 100; }
};

#endif // EQUALIZERNODE_HPP
