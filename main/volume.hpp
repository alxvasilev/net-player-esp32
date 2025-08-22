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
    // volume is in percent (0-100%) of original.
    virtual uint8_t getVolume() const { return (mVolume * 100 + kVolumeDiv / 2) >> kVolumeDivShift; }
    virtual void setVolume(uint8_t vol) {
        mVolume = ((int)vol * kVolumeDiv) / 100;
        mFloatVolumeMul = ((float)vol) / 100;
        ESP_LOGI("vol", "Setting volume to %d%% (float: %.2f, int: %u/%u)", vol, mFloatVolumeMul, mVolume, kVolumeDiv);
    }
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
    enum { kVolumeDiv = 256, kVolumeDivShift = 8 };
    StereoLevels mAudioLevels;
    AudioLevelCallbck mAudioLevelCb = nullptr;
    void* mAudioLevelCbArg = nullptr;
    float mFloatVolumeMul = 0.01;
    uint16_t mVolume = kVolumeDiv; // 16-bit because it can be 256
    uint8_t mVolLevelMeasurePoint = -1;
    void volumeNotifyLevelCallback() {
        mAudioLevelCb(mAudioLevelCbArg);
    }
};
#endif
