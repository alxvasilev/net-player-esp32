#ifndef VU_DISPLAY_H
#define VU_DISPLAY_H
#include <stdint.h>
#include <limits>
#include "volume.hpp"

class ST7735Display;
class NvsHandle;

class VuDisplay {
    enum {
        kLevelSmoothFactor = 4, kDefPeakHoldMs = 30, kDefPeakDropSpeed = 20,
        kDefLedWidth = 1, kDefLedHeight = 10, kDefLedSpacing = 1, kDefChanSpacing = 2
    };
    enum: uint16_t {
        kLevelMax = std::numeric_limits<int16_t>::max(),
        kDefYellowThresh = kLevelMax - 512
    };
    struct ChanCtx
    {
        int16_t barY;
        int16_t avgLevel;
        int16_t peakLevel;
        int16_t prevBarLen;
        uint8_t peakTimer;
        void reset() { avgLevel = peakLevel = prevBarLen = peakTimer = 0; }
        ChanCtx() { reset(); }
    };
    ST7735Display& mLcd;
    ChanCtx mLeftCtx;
    ChanCtx mRightCtx;
    int8_t mStepWidth; // mLedWidth + led spacing
    int8_t mLedWidth;
    int8_t mLedHeight;
    int8_t mChanSpacing;
    int16_t mYellowStartX;
    uint16_t mGreenColor;
    uint16_t mYellowColor;
    int32_t mLevelPerLed;
    uint8_t mPeakDropTicks;
    uint8_t mPeakHoldTicks;
    inline uint16_t ledColor(int16_t ledX, int16_t level);
    void calculateLevels(ChanCtx& ctx, int16_t level);
    void drawChannel(ChanCtx& ctx, int16_t level);
    inline int16_t numLedsForLevel(int16_t level);
public:
    VuDisplay(ST7735Display& lcd): mLcd(lcd) {}
    void init(NvsHandle& nvs);
    void update(const IAudioVolume::StereoLevels& levels);
    void reset(NvsHandle& nvs);
};

#endif
