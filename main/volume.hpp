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
    void volEnableLevel(AudioLevelCallbck cb, void* arg)
    {
        mAudioLevelCb = cb;
        mAudioLevelCbArg = arg;
    }
    bool volLevelEnabled() const { return mAudioLevelCb != nullptr; }
    void volEnableProcessing(bool ena) { mVolProcessingEnabled = ena; }
    bool volProcessingEnabled() const { return mVolProcessingEnabled; }
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
    bool mVolProcessingEnabled = false;
};

class DefaultVolumeImpl: public IAudioVolume
{
protected:
    enum: uint8_t { kVolumeDivShift = 7, kVolumeDiv = 1 << kVolumeDivShift };
    uint8_t mVolume = kVolumeDiv;
private:
    typedef void (DefaultVolumeImpl::*ProcessFunc)(AudioNode::DataPullReq& dpr);
    ProcessFunc mProcessVolumeFunc = nullptr;
    ProcessFunc mGetLevelFunc = nullptr;
    StreamFormat mFormat;
template <typename T>
void processVolume(AudioNode::DataPullReq& dpr)
{
    T* end = (T*)(dpr.buf + dpr.size);
    for(T* pSample = (T*)dpr.buf; pSample < end; pSample++) {
        *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) >> kVolumeDivShift;
    }
}
template <typename T>
void getPeakLevelMono(AudioNode::DataPullReq& dpr)
{
    T level = 0;
    T* end = (T*)(dpr.buf + dpr.size);
    for(T* pSample = (T*)dpr.buf; pSample < end; pSample++) {
        if (*pSample > level) {
            level = *pSample;
        }
    }
    // sample is always left-aligned. I.e. for 24 bit, we have a left-aligned 32bit int
    enum { kShift = std::max((int)sizeof(T) * 8 - 16, 0) };
    if (kShift != 0) {
        mAudioLevels.left = mAudioLevels.right = level >> kShift;
    } else {
        mAudioLevels.left = mAudioLevels.right = level;
    }
}
template <typename T>
void getPeakLevelStereo(AudioNode::DataPullReq& dpr)
{
    T left = 0;
    T right = 0;
    T* end = (T*)(dpr.buf + dpr.size);
    for(T* pSample = (T*)dpr.buf; pSample < end; pSample++) {
        if (*pSample > right) {
            right = *pSample;
        }
        pSample++;
        if (*pSample > left) {
            left = *pSample;
        }
    }
    enum { kShift = std::max((int)sizeof(T) * 8 - 16, 0) };
    if (kShift != 0) {
        mAudioLevels.left = left >> kShift;
        mAudioLevels.right = right >> kShift;
    } else {
        mAudioLevels.left = left;
        mAudioLevels.right = right;
    }
}

template<typename T>
void updateProcessFuncsStereo()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolume<T>;
    mGetLevelFunc = &DefaultVolumeImpl::getPeakLevelStereo<T>;
}
template<typename T>
void updateProcessFuncsMono()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolume<T>;
    mGetLevelFunc = &DefaultVolumeImpl::getPeakLevelMono<T>;
}
bool updateVolumeFormat(StreamFormat fmt)
{
    auto bps = fmt.bitsPerSample();
    auto nChans = fmt.numChannels();
    if (nChans == 2) {
        if (bps == 16) {
            updateProcessFuncsStereo<int16_t>();
        } else if (bps == 24) {
            updateProcessFuncsStereo<int32_t>();
        } else if (bps == 32) {
            updateProcessFuncsStereo<int32_t>();
        } else if (bps == 8) {
            updateProcessFuncsStereo<int8_t>();
        } else {
            return false;
        }
    } else if (nChans == 1) {
        if (bps == 16) {
            updateProcessFuncsMono<int16_t>();
        } else if (bps == 24) {
            updateProcessFuncsMono<int32_t>();
        } else if (bps == 32) {
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
    if (dpr.fmt != mFormat) {
        updateVolumeFormat(dpr.fmt);
    }
    (this->*mGetLevelFunc)(dpr);
}
void volumeNotifyLevelCallback()
{
    mAudioLevelCb(mAudioLevelCbArg);
}
public:
uint16_t getVolume() const
{
    return (mVolume * 100 + kVolumeDiv/2) >> kVolumeDivShift;
}

void setVolume(uint16_t vol)
{
    mVolume = ((vol * kVolumeDiv + 50) / 100);
}
};
#endif
