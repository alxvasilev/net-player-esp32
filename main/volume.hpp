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
    virtual uint8_t getVolume() const = 0;
    virtual void setVolume(uint8_t vol) = 0;
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
    uint8_t mVolumeAndAlignShift = 0;
private:
    typedef void (DefaultVolumeImpl::*ProcessFunc)(AudioNode::DataPullReq& dpr);
    ProcessFunc mProcessVolumeFunc = nullptr;
    ProcessFunc mGetLevelFunc = nullptr;
    StreamFormat mFormat;
    static constexpr int calculateShift(int bitsPerSample)
    {
        if (bitsPerSample == 8 || bitsPerSample == 24) {
            return kVolumeDivShift - 8;
        } else {
            // static_assert(bitsPerSample == 16 || bitsPerSample == 32, "Unsupported bits per sample");
            return kVolumeDivShift;
        }
    }
template <typename T, int Bps>
void processVolume(AudioNode::DataPullReq& dpr)
{
    // we do two shifts at once - one is for the volume multiply/divide, and the other one
    // is to left-align the sample, as is required by i2s
    enum { kShift = calculateShift(Bps) };
    T* end = (T*)(dpr.buf + dpr.size);
    for(T* pSample = (T*)dpr.buf; pSample < end; pSample++) {
        int32_t unaligned = (static_cast<int32_t>(*pSample) * mVolume + kVolumeDiv / 2);
        *pSample = (kShift >= 0) ? (unaligned >> kShift) : (unaligned << -kShift);
    }
    dpr.fmt.setIsLeftAligned(true);
}

template <typename T, int Bps, bool LeftAligned>
void getPeakLevelMono(AudioNode::DataPullReq& dpr)
{
    T level = 0;
    T* end = (T*)(dpr.buf + dpr.size);
    for(T* pSample = (T*)dpr.buf; pSample < end; pSample++) {
        if (*pSample > level) {
            level = *pSample;
        }
    }
    enum { kShift = LeftAligned
           ? std::max((int)sizeof(T) * 8 - 16, 0)
           : std::max(Bps - 16, 0)
    };
    if (kShift != 0) {
        mAudioLevels.left = mAudioLevels.right = level >> kShift;
    } else {
        mAudioLevels.left = mAudioLevels.right = level;
    }
}

template <typename T, int Bps, bool LeftAligned>
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
    // leave only the most significant 16 bits
    enum { kShift = LeftAligned
           ? std::max((int)sizeof(T) * 8 - 16, 0)
           : std::max(Bps - 16, 0)
    };
    if (kShift != 0) {
        mAudioLevels.left = left >> kShift;
        mAudioLevels.right = right >> kShift;
    } else {
        mAudioLevels.left = left;
        mAudioLevels.right = right;
    }
}

template<typename T, int Bps>
void updateProcessFuncsStereo()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolume<T, Bps>;
    mGetLevelFunc = mFormat.isLeftAligned()
        ? &DefaultVolumeImpl::getPeakLevelStereo<T, Bps, true>
        : &DefaultVolumeImpl::getPeakLevelStereo<T, Bps, false>;
}

template<typename T, int Bps>
void updateProcessFuncsMono()
{
    mProcessVolumeFunc = &DefaultVolumeImpl::processVolume<T, Bps>;
    mGetLevelFunc = mFormat.isLeftAligned()
        ? &DefaultVolumeImpl::getPeakLevelMono<T, Bps, true>
        : &DefaultVolumeImpl::getPeakLevelMono<T, Bps, false>;
}
template <typename T, int Bps>
bool updateProcessFuncs()
{
    auto nChans = mFormat.numChannels();
    if (nChans == 2) {
        updateProcessFuncsStereo<T, Bps>();
    } else if (nChans == 1) {
        updateProcessFuncsMono<T, Bps>();
    } else {
        return false;
    }
    return true;
}

bool updateVolumeFormat(StreamFormat fmt)
{
    mFormat = fmt;
    auto bps = fmt.bitsPerSample();
    if (bps == 16) {
        return updateProcessFuncs<int16_t, 16>();
    } else if (bps == 24) {
        return updateProcessFuncs<int32_t, 24>();
    } else if (bps == 32) {
        return updateProcessFuncs<int32_t, 32>();
    } else if (bps == 8) {
        return updateProcessFuncs<int16_t, 8>();
    } else {
        return false;
    }
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
uint8_t getVolume() const
{
    return (mVolume * 100 + kVolumeDiv/2) >> kVolumeDivShift;
}

void setVolume(uint8_t vol)
{
    mVolume = ((vol * kVolumeDiv + 50) / 100);
}
};
#endif
