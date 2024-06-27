#ifndef VOLUME_HPP_INCLUDED
#define VOLUME_HPP_INCLUDED

#include "audioNode.hpp"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include <type_traits>
#include <limits>


/* Interface for setting and getting volume of an audio node. If implemented
 * by an audio node, flags() should have the kHasVolume bit set.
 * NOTE: Most of the methods are not virtual for performance reasons, which
 * necessitates also the inclusion of (the public) part of the implementation here.
 */
class IAudioVolume
{
public:
    // The actual levels are in the range [0-256].
    // The most significant 8 bits of the max sample are taken, and, if
    // the sample has the maximum possible value, the level is set to 256
    struct StereoLevels
    {
        union {
            struct {
                int16_t left;
                int16_t right;
            };
            uint32_t data;
        };
    };
    typedef void(*AudioLevelCallbck)(void* arg);
    void volEnableLevel(AudioLevelCallbck cb, void* arg, uint8_t measurePoint)
    {
        mAudioLevelCb = cb;
        mAudioLevelCbArg = arg;
        mVolLevelMeasurePoint = measurePoint;
    }
    void volDisableLevel() {
        mAudioLevelCb = nullptr;
        mAudioLevelCbArg = nullptr;
        mVolLevelMeasurePoint = -1;
    }
    void volSetMeasurePoint(uint8_t point) { mVolLevelMeasurePoint = point; }
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
        mAudioLevels.data = 0;
        if (mAudioLevelCb) {
            mAudioLevelCb(mAudioLevelCbArg);
        }
    }
    void clearAudioLevelsNoEvent() { mAudioLevels.data = 0; }
protected:
    StereoLevels mAudioLevels;
    AudioLevelCallbck mAudioLevelCb = nullptr;
    void* mAudioLevelCbArg = nullptr;
    int8_t mVolLevelMeasurePoint = -1;
    bool mVolProcessingEnabled = false;
};

class DefaultVolumeImpl: public IAudioVolume
{
protected:
    enum: uint8_t { kVolumeDivShift = 8 };
    enum: uint16_t { kVolumeDiv = 1 << kVolumeDivShift };
    uint16_t mVolume = kVolumeDiv; // 16-bit because it can be 256
private:
    typedef void (DefaultVolumeImpl::*ProcessFunc)(DataPacket& pkt);
    ProcessFunc mProcessVolumeFunc = nullptr;
    ProcessFunc mGetLevelFunc = nullptr;
    StreamFormat mFormat;
    uint8_t mGetLevelLeftAlignedFlag = 0; // we are configured for left-aligned samples in volumeLevelGet()
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
void processVolume(DataPacket& pkt)
{
    // we do two shifts at once - one is for the volume multiply/divide, and the other one
    // is to left-align the sample, as is required by i2s
    enum { kShift = calculateShift(Bps) };
    T* end = (T*)(pkt.data + pkt.dataLen);
    for(T* pSample = (T*)pkt.data; pSample < end; pSample++) {
        int32_t unaligned = (static_cast<int32_t>(*pSample) * mVolume + kVolumeDiv / 2);
        *pSample = (kShift >= 0) ? (unaligned >> kShift) : (unaligned << -kShift);
    }
    pkt.flags |= StreamPacket::kFlagLeftAlignedSamples;
}
void processVolume32(DataPacket& pkt)
{
    // we do two shifts at once - one is for the volume multiply/divide, and the other one
    // is to left-align the sample, as is required by i2s
    int32_t* end = (int32_t*)(pkt.data + pkt.dataLen);
    for(int32_t* pSample = (int32_t*)pkt.data; pSample < end; pSample++) {
        *pSample = (static_cast<int64_t>(*pSample) * mVolume + kVolumeDiv / 2) >> kVolumeDivShift;
    }
    pkt.flags |= StreamPacket::kFlagLeftAlignedSamples;
}

template <typename T, int Bps, bool LeftAligned>
void getPeakLevelMono(DataPacket& pkt)
{
    T level = 0;
    T* end = (T*)(pkt.data + pkt.dataLen);
    for(T* pSample = (T*)pkt.data; pSample < end; pSample++) {
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
void getPeakLevelStereo(DataPacket& pkt)
{
    T left = 0;
    T right = 0;
    T* end = (T*)(pkt.data + pkt.dataLen);
    for(T* pSample = (T*)pkt.data; pSample < end; pSample++) {
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
void updateProcessFuncsStereo(bool onlyGetLevel)
{
    if (!onlyGetLevel) {
        mProcessVolumeFunc = (Bps == 32)
            ? &DefaultVolumeImpl::processVolume32
            : &DefaultVolumeImpl::processVolume<T, Bps>;
    }
    mGetLevelFunc = mGetLevelLeftAlignedFlag
        ? &DefaultVolumeImpl::getPeakLevelStereo<T, Bps, true>
        : &DefaultVolumeImpl::getPeakLevelStereo<T, Bps, false>;
}

template<typename T, int Bps>
void updateProcessFuncsMono(bool onlyGetLevel)
{
    if (!onlyGetLevel) {
        mProcessVolumeFunc = (Bps == 32)
            ? &DefaultVolumeImpl::processVolume32
            : &DefaultVolumeImpl::processVolume<T, Bps>;
    }
    mGetLevelFunc = mGetLevelLeftAlignedFlag
        ? &DefaultVolumeImpl::getPeakLevelMono<T, Bps, true>
        : &DefaultVolumeImpl::getPeakLevelMono<T, Bps, false>;
}
template <typename T, int Bps>
bool updateProcessFuncs(bool onlyGetLevel)
{
    auto nChans = mFormat.numChannels();
    if (nChans == 2) {
        updateProcessFuncsStereo<T, Bps>(onlyGetLevel);
    } else if (nChans == 1) {
        updateProcessFuncsMono<T, Bps>(onlyGetLevel);
    } else {
        return false;
    }
    return true;
}

bool updateVolumeFormat(StreamFormat fmt, bool leftAligned)
{
    bool onlyGetLevel;
    if (mFormat == fmt) {
        onlyGetLevel = true;
    }
    else {
        mFormat = fmt;
        onlyGetLevel = false;
    }
    mGetLevelLeftAlignedFlag = leftAligned ? StreamPacket::kFlagLeftAlignedSamples : 0;
    auto bps = fmt.bitsPerSample();
    if (bps == 16) {
        return updateProcessFuncs<int16_t, 16>(onlyGetLevel);
    } else if (bps == 24) {
        return updateProcessFuncs<int32_t, 24>(onlyGetLevel);
    } else if (bps == 32) {
        return updateProcessFuncs<int32_t, 32>(onlyGetLevel);
    } else if (bps == 8) {
        return updateProcessFuncs<int16_t, 8>(onlyGetLevel);
    } else {
        return false;
    }
}
protected:
void volumeProcess(DataPacket& pkt)
{
    if (mVolume != kVolumeDiv) { // 100% volume, nothing to change
        (this->*mProcessVolumeFunc)(pkt);
    }
}
void volumeUpdateFormat(StreamFormat fmt)
{
    if (fmt != mFormat) {
        updateVolumeFormat(fmt, false);
    }
}
void volumeGetLevel(DataPacket& pkt)
{
    uint8_t laFlag = (pkt.flags & StreamPacket::kFlagLeftAlignedSamples);
    if (laFlag != mGetLevelLeftAlignedFlag) {
        updateVolumeFormat(mFormat, laFlag);
    }
    (this->*mGetLevelFunc)((DataPacket&)pkt);
}
void volumeNotifyLevelCallback()
{
    mAudioLevelCb(mAudioLevelCbArg);
}
public:
// volume is a value 0-100
uint8_t getVolume() const
{
    return (mVolume * 100 + kVolumeDiv / 2) >> kVolumeDivShift;
}

void setVolume(uint8_t vol)
{
    mVolume = ((vol * kVolumeDiv) / 100);
    ESP_LOGI("vol", "Setting volume multiplier to %u / %u", mVolume, kVolumeDiv);
}
};
#endif
