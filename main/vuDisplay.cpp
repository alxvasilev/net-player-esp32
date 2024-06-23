#include "vuDisplay.hpp"
#include <st7735.hpp>
#include "nvsHandle.hpp"
#include <esp_log.h>

static const char* TAG = "vu";

void VuDisplay::init(NvsHandle& nvs)
{
    mLedWidth = nvs.readDefault<uint8_t>("vuLedWidth", kDefLedWidth);
    mStepWidth = mLedWidth + nvs.readDefault<uint8_t>("vuLedSpacing", kDefLedSpacing);
    if (mLcd.width() % mStepWidth) {
        ESP_LOGE(TAG, "Specified VU led width %d is not a divisor of display width. VU meter will not behave correctly", mStepWidth);
    }
    int16_t numLeds = mLcd.width() / mStepWidth;
    mLevelPerLed = (100 * int32_t(kLevelMax + 1) + numLeds / 2) / numLeds; // 100x the rounded division, i.e. fixed point two-decimal precision
    mLedHeight = nvs.readDefault<uint8_t>("vuLedHeight", kDefLedHeight);
    mChanSpacing = nvs.readDefault<uint8_t>("vuChanSpacing", kDefChanSpacing);
    mYellowStartX = mStepWidth * (100 * (int32_t)nvs.readDefault<int16_t>("vuYellowThresh", kDefYellowThresh)) / mLevelPerLed;
    enum { kTicksPerSec = 43 }; // ~1024 samples at 44100 Hz
    auto pkDropSpeed = nvs.readDefault<uint8_t>("vuPeakDropSpeed", kDefPeakDropSpeed);
    mPeakDropTicks = (kTicksPerSec * mStepWidth + pkDropSpeed / 2) / pkDropSpeed;
    auto pkHoldMs = nvs.readDefault<uint16_t>("vuPeakHoldMs", 500);
    mPeakHoldTicks = (pkHoldMs + (1000 / kTicksPerSec) / 2) / (1000 / kTicksPerSec);
    mGreenColor = nvs.readDefault<uint16_t>("vuClrGreen", Color565::GREEN);
    mYellowColor = nvs.readDefault<uint16_t>("vuClrYellow", Color565::YELLOW);
    ESP_LOGD(TAG, "init: stepWidth: %d, ledWidth: %d, levelPerLed: %f, yellowStartX: %d", mStepWidth, mLedWidth, mYellowStartX / (float)100, mYellowStartX);
    mLeftCtx.barY = mLcd.height() - 2 * mLedHeight - mChanSpacing;
    mRightCtx.barY = mLeftCtx.barY + mLedHeight + mChanSpacing;
}

void VuDisplay::update(const IAudioVolume::StereoLevels& levels)
{
    drawChannel(mLeftCtx, levels.left);
    drawChannel(mRightCtx, levels.right);
}

inline Color565 VuDisplay::ledColor(int16_t ledX, int16_t level)
{
    if (ledX < mYellowStartX) {
        return mGreenColor;
    } else {
        return (level >= kLevelMax) ? Color565::RED : mYellowColor;
    }
}

void VuDisplay::calculateLevels(ChanCtx& ctx, int16_t level)
{
    if (level > ctx.avgLevel) {
        ctx.avgLevel = level;
    } else {
        ctx.avgLevel = ((int32_t)ctx.avgLevel * (kLevelSmoothFactor-1) + level + kLevelSmoothFactor / 2) / kLevelSmoothFactor;
    }
    if (level >= ctx.peakLevel) {
        ctx.peakLevel = level;
        ctx.peakTimer = mPeakHoldTicks;
    }
    else { // peak is above curent level
        if (--ctx.peakTimer <= 0) {
            ctx.peakTimer = mPeakDropTicks;
            if (ctx.peakLevel > 0) {
                ctx.peakLevel -= mLevelPerLed / 100;
                if (ctx.peakLevel < 0) {
                    ctx.peakLevel = 0;
                }
            }
        }
    }
}

inline int16_t VuDisplay::numLedsForLevel(int16_t level)
{
    // Rounded-up division of (level * 100) / mLevelPerLed
    return (100 * (int32_t)level + mLevelPerLed - 1) / mLevelPerLed;
}
void VuDisplay::drawChannel(ChanCtx& ctx, int16_t level)
{
    calculateLevels(ctx, level);
    // Width in pixels of level bar.
    auto nCurrLeds = numLedsForLevel(ctx.avgLevel);
    int16_t levelBarLen = mStepWidth * nCurrLeds;
    // Draw bar
    for (int16_t x = ctx.prevBarLen; x < levelBarLen; x += mStepWidth) {
        mLcd.setFgColor(ledColor(x, ctx.peakLevel)); // keep red color for "yellow" part, as long as peak-hold is at kMaxLevel
        mLcd.fillRect(x, ctx.barY, mLedWidth, mLedHeight);
    }
    ctx.prevBarLen = levelBarLen;

    auto nPeakLeds = numLedsForLevel(ctx.peakLevel);
    // draw peak indicator and background before and after it
    if (nPeakLeds <= nCurrLeds) { // no peak led after level bar
        int16_t afterLen = mLcd.width() - levelBarLen;
        if (afterLen > 0) {
            // draw background after level bar and return
            mLcd.clear(levelBarLen, ctx.barY, afterLen, mLedHeight);
        }
        return;
    }

    int16_t peakLedX = mStepWidth * (nPeakLeds - 1);
    if (peakLedX > levelBarLen) {
        // draw background between end of level bar and peak led
        mLcd.clear(levelBarLen, ctx.barY, peakLedX - levelBarLen, mLedHeight);
    }
    mLcd.setFgColor(ledColor(peakLedX, ctx.peakLevel));
    // draw peak led
    mLcd.fillRect(peakLedX, ctx.barY, mLedWidth, mLedHeight);
    // draw background after peak led
    auto peakLedEndX = peakLedX + mStepWidth;
    int16_t afterLen = mLcd.width() - peakLedEndX;
    if (afterLen > 0) {
        mLcd.clear(peakLedEndX, ctx.barY, afterLen, mLedHeight);
    }
}
void VuDisplay::reset(NvsHandle &nvs)
{
    mLeftCtx.reset();
    mRightCtx.reset();
    init(nvs);
    IAudioVolume::StereoLevels levels;
    levels.left = levels.right = 0;
    update(levels);
}
