#ifndef VU_DISPLAY_H
#define VU_DISPLAY_H
#include <stdint.h>
#include <limits>
#include "volume.hpp"
#include <lcdColor.hpp>

template <class C>
class FrameBufferColor;
template <class Fb>
class Lcd;
typedef Lcd<FrameBufferColor<Color565>> LcdFrameBuf;

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
        uint8_t peakTimer;
        void reset() { avgLevel = peakLevel = peakTimer = 0; }
        ChanCtx() { reset(); }
    };
    LcdFrameBuf& mLcd;
    ChanCtx mLeftCtx;
    ChanCtx mRightCtx;
    int8_t mStepWidth; // mLedWidth + led spacing
    int8_t mLedWidth;
    int8_t mLedHeight;
    int8_t mChanSpacing;
    int16_t mYellowStartX;
    Color565 mGreenColor;
    Color565 mYellowColor;
    int32_t mLevelPerLed;
    uint8_t mPeakDropTicks;
    uint8_t mPeakHoldTicks;
    uint8_t mHeight;
    inline Color565 ledColor(int16_t ledX, int16_t level);
    void calculateLevels(ChanCtx& ctx, int16_t level);
    void drawChannel(ChanCtx& ctx, int16_t level);
    inline int16_t numLedsForLevel(int16_t level);
public:
    VuDisplay(LcdFrameBuf& lcd): mLcd(lcd) {}
    void init(NvsHandle& nvs);
    void update(const IAudioVolume::StereoLevels& levels);
    void reset(NvsHandle& nvs);
    int16_t height() const { return mHeight; }
};

#endif
