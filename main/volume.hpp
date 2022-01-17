#ifndef VOLUME_HPP_INCLUDED
#define VOLUME_HPP_INCLUDED

#include "audioNode.hpp"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include <type_traits>
#include <limits>


/* Interface for setting and getting volume of an audio node. If implemented,
 * flags() should have the kHasVolume bit set.
 * IMPORTANT: This interface class should have no members, because it is
 * used in multple inheritance and AudioNode is cast to IAudioVolume based
 * on the presence of kHasVolume bit in flags(). This cast will not work correctly
 * if IAudioVolume is not a pure virtual class, because the this pointers will
 * differ in the multiple inheritance
 */
class IAudioVolume
{
public:
    // The actual levels are in the range [0-256].
    // The most significant 8 bits of the max sample are taken, and, if
    // the sample has the maximum possible value, the level is set to 256
    struct StereoLevels
    {
        int16_t left;
        int16_t right;
    };
    typedef void(*AudioLevelCallbck)(void* arg);
    void setLevelCallback(AudioLevelCallbck cb, void* arg)
    {
        mAudioLevelCb = cb;
        mAudioLevelCbArg = arg;
    }
    // volume is in percent of original.
    // 0-99% attenuates, 101-400% amplifies
    virtual uint16_t getVolume() const = 0;
    virtual void setVolume(uint16_t vol) = 0;
    const StereoLevels& audioLevels() const { return mAudioLevels; }
    void clearAudioLevels()
    {
        mAudioLevels.left = mAudioLevels.right = 0;
        if (mAudioLevelCb) {
            mAudioLevelCb(mAudioLevelCbArg);
        }
    }
    void clearAudioLevelsNoEvent() { mAudioLevels.left = mAudioLevels.right = 0; }
protected:
    StereoLevels mAudioLevels;
    AudioLevelCallbck mAudioLevelCb = nullptr;
    void* mAudioLevelCbArg = nullptr;
};

class DefaultVolumeImpl: public IAudioVolume
{
protected:
    enum: uint8_t { kVolumeDiv = 64 };
    uint8_t mVolume = kVolumeDiv;
private:
template <typename T, bool ChangeVol, bool GetLevel>
void processVolumeStereo(AudioNode::DataPullReq& dpr)
{
    T left = 0;
    T right = 0;
    T* pSample = (T*)dpr.buf;
    T* end = pSample + dpr.size / sizeof(T);
    while(pSample < end) {
        if (GetLevel) {
            if (*pSample > left) {
                left = *pSample;
            }
        }
        if (ChangeVol) {
            *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) / kVolumeDiv;
        }
        pSample++;

        if (GetLevel) {
            if (*pSample > right) {
                right = *pSample;
            }
        }
        if (ChangeVol) {
            *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) / kVolumeDiv;
        }
        pSample++;
    }
    if (GetLevel) {
        mAudioLevels.left = left;
        mAudioLevels.right = right;
    }
}

template <typename T, bool ChangeVol, bool GetLevel>
void processVolumeMono(AudioNode::DataPullReq& dpr)
{
    T level = 0;
    T* pSample = (T*)dpr.buf;
    T* end = pSample + dpr.size / sizeof(T);
    for(; pSample < end; pSample++) {
        if (GetLevel) {
            if (*pSample > level) {
                level = *pSample;
            }
        }

        if (ChangeVol) {
            *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) / kVolumeDiv;
        }
    }
    if (GetLevel) {
        mAudioLevels.left = mAudioLevels.right = level;
    }
}
protected:
void processVolumeAndLevel(AudioNode::DataPullReq& dpr)
{
    if (mVolume != kVolumeDiv) {
        if (dpr.fmt.isStereo()) {
            processVolumeStereo<int16_t, true, true>(dpr);
        } else {
            processVolumeMono<int16_t, true, true>(dpr);
        }
        if (mAudioLevelCb) {
            mAudioLevelCb(mAudioLevelCbArg);
        }
    } else if (mAudioLevelCb) { // only find peak amplitude
        if (dpr.fmt.isStereo()) {
            processVolumeStereo<int16_t, false, true>(dpr);
        } else {
            processVolumeMono<int16_t, false, true>(dpr);
        }
        mAudioLevelCb(mAudioLevelCbArg);
    }
}

void processVolume(AudioNode::DataPullReq& dpr)
{
    if (mVolume == kVolumeDiv) {
        return;
    }
    if (dpr.fmt.isStereo()) {
        processVolumeStereo<int16_t, true, false>(dpr);
    } else {
        processVolumeMono<int16_t, true, false>(dpr);
    }
}

void getAudioLevel(AudioNode::DataPullReq& dpr)
{
    if (!mAudioLevelCb) { // only find peak amplitude
        return;
    }
    if (dpr.fmt.isStereo()) {
        processVolumeStereo<int16_t, false, true>(dpr);
    } else {
        processVolumeMono<int16_t, false, true>(dpr);
    }
    mAudioLevelCb(mAudioLevelCbArg);
}

uint16_t getVolume() const
{
    return (mVolume * 100 + kVolumeDiv/2) / kVolumeDiv;
}

void setVolume(uint16_t vol)
{
    mVolume = ((vol * kVolumeDiv + 50) / 100);
}
};
#endif
