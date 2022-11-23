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
    typedef void (DefaultVolumeImpl::*ProcessFunc)(AudioNode::DataPullReq& dpr);
    ProcessFunc mProcessVolumeFunc = nullptr;
    ProcessFunc mGetLevelFunc = nullptr;
    StreamFormat mFormat;
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
template<typename T>
void updateProcessFuncsStereo()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolumeStereo<T, true, false>;
    mGetLevelFunc = &DefaultVolumeImpl::processVolumeStereo<T, false, true>;
}
template<typename T>
void updateProcessFuncsMono()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolumeMono<T, true, false>;
    mGetLevelFunc = &DefaultVolumeImpl::processVolumeMono<T, false, true>;
}
bool updateVolumeFormat(StreamFormat fmt)
{
    auto bps = fmt.bitsPerSample();
    auto nChans = fmt.numChannels();
    if (nChans == 2) {
        if (bps == 16) {
            updateProcessFuncsStereo<int16_t>();
        } else if (bps == 24 || bps == 32) {
            updateProcessFuncsStereo<int32_t>();
        } else if (bps == 8) {
            updateProcessFuncsStereo<int8_t>();
        } else {
            return false;
        }
    } else if (nChans == 1) {
        if (bps == 16) {
            updateProcessFuncsMono<int16_t>();
        } else if (bps == 24 || bps == 32) {
            updateProcessFuncsMono<int32_t>();
        } else if (bps == 8) {
            updateProcessFuncsMono<int8_t>();
        } else {
            return false;
        }
    } else {
        return false;
    }
    return true;
}
protected:
void volumeProcess(AudioNode::DataPullReq& dpr)
{
    if (mVolume == kVolumeDiv) {
        return;
    }
    if (dpr.fmt != mFormat) {
        updateVolumeFormat(dpr.fmt);
    }
    (this->*mProcessVolumeFunc)(dpr);
}
void volumeGetLevel(AudioNode::DataPullReq& dpr)
{
    if (!mAudioLevelCb) { // only find peak amplitude
        return;
    }
    if (dpr.fmt != mFormat) {
        updateVolumeFormat(dpr.fmt);
    }
    (this->*mGetLevelFunc)(dpr);
    mAudioLevelCb(mAudioLevelCbArg);
}
public:
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
