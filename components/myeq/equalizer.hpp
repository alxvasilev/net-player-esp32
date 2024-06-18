#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <array>
#include <esp_heap_caps.h>

struct EqBandConfig {
    uint16_t freq;
    int8_t width; // in 0.1x octaves: 10 means 1 octave
    static const EqBandConfig kPreset10Band[10];
    static const EqBandConfig kPreset9Band[9];
    static const EqBandConfig kPreset8Band[8];
    static const EqBandConfig kPreset7Band[7];
    static const EqBandConfig kPreset6Band[6];
    static const EqBandConfig kPreset5Band[5];
    static const EqBandConfig kPreset4Band[4];
    static const EqBandConfig kPreset3Band[3];
    static const EqBandConfig* kBandPresets[8];
    static const EqBandConfig* defaultForNBands(int n) {
        return (n < 3 || n > 10) ? nullptr : kBandPresets[n - 3];
    }
};

template <int N, bool IsStereo>
class Equalizer
{
public:
    enum { kBandCount = N };
    typedef typename std::conditional<IsStereo, BiquadStereo, BiquadMono>::type BiquadType;
protected:
    const EqBandConfig* mBandConfigs;
    BiquadType mFilters[kBandCount];
    int mSampleRate;
public:
    Equalizer(const EqBandConfig* cfg)
        : mBandConfigs(cfg ? cfg : EqBandConfig::defaultForNBands(kBandCount))
    {
        mFilters[0].init((mBandConfigs[0].width < 0) ? Biquad::kLowShelf : Biquad::kBand);
        mFilters[kBandCount - 1].init((mBandConfigs[kBandCount - 1].width < 0) ? Biquad::kHighShelf : Biquad::kBand);
        for (int i = 1; i < kBandCount - 1; i++) {
            mFilters[i].init(Biquad::kBand);
        }
    }
    static void* operator new(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_INTERNAL); }
    static void operator delete(void* p) { free(p); }
    const EqBandConfig* bandConfigs() const { return mBandConfigs; }
    const BiquadType& filter(uint8_t band) const
    {
        bqassert(band < kBandCount);
        return mFilters[band];
    }
    void init(int sr, int8_t* gains)
    {
        mSampleRate = sr;
        if (gains) {
            for (int i = 0; i < kBandCount; i++) {
                setBandGain(i,  gains[i], true);
            }
        } else {
            for (int i = 0; i < kBandCount; i++) {
                setBandGain(i, 0, true);
            }
        }
    }
    void resetState()
    {
        for (int i = 0; i < kBandCount; i++) {
            mFilters[i].clearState();
        }
    }
    void process(float* input, int len)
    {
#pragma GCC unroll 16
        for (int i = 0; i < kBandCount; i++) {
            mFilters[i].process(input, len);
        }
    }
    void setBandGain(uint8_t band, int8_t dbGain, bool clearState=false)
    {
        bqassert(band < kBandCount);
        auto& filter = mFilters[band];
        auto& cfg = mBandConfigs[band];
        filter.set(cfg.freq, ((float)abs(cfg.width)) / 10, mSampleRate, dbGain);
        if (clearState) {
            filter.clearState();
        }
    }
    void setAllGains(const int8_t* gains)
    {
        if (gains) {
            for (int i = 0; i < kBandCount; i++) {
                auto& cfg = mBandConfigs[i];
                mFilters[i].set(cfg.freq, ((float)abs(cfg.width)) / 10, mSampleRate, gains[i]);
            }
        } else {
            for (int i = 0; i < kBandCount; i++) {
                setBandGain(i, 0, true);
            }
        }
    }
};
#endif // EQUALIZERNODE_HPP
