#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <array>

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

template <int N, typename S>
class Equalizer
{
public:
    typedef S Sample;
    enum { kBandCount = N };
protected:
    const EqBandConfig* mBandConfigs;
    BiQuad<Sample> mFilters[kBandCount];
    int mSampleRate;
public:
    Equalizer(const EqBandConfig* cfg)
        : mBandConfigs(cfg ? cfg : EqBandConfig::defaultForNBands(kBandCount))
    {
        mFilters[0].init((mBandConfigs[0].width < 0) ? BiQuadType::LSH : BiQuadType::PEQ);
        mFilters[kBandCount - 1].init((mBandConfigs[kBandCount - 1].width < 0) ? BiQuadType::HSH : BiQuadType::PEQ);
        for (int i = 1; i < kBandCount - 1; i++) {
            mFilters[i].init(BiQuadType::PEQ);
        }
    }
    const EqBandConfig* bandConfigs() const { return mBandConfigs; }
    const BiQuad<Sample>& filter(uint8_t band) const
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
    Equalizer::Sample process(Sample sample)
    {
#pragma GCC unroll 16
        for (int i = 0; i< kBandCount; i++) {
            sample = mFilters[i].process(sample);
        }
        return sample;
    }
    template<typename T>
    T processAndNarrow(T sample) {
        Sample s = process(sample);
        if (s > std::numeric_limits<T>::max()) {
            s = std::numeric_limits<T>::max();
        } else if (s < std::numeric_limits<T>::min()) {
            s = std::numeric_limits<T>::min();
        } else if (std::is_integral_v<T> && !std::is_integral_v<Sample>) {
            s = roundf(s);
        }
        return s;
    }
    void setBandGain(uint8_t band, int8_t dbGain, bool clearState=false)
    {
        bqassert(band < kBandCount);
        auto& filter = mFilters[band];
        auto& cfg = mBandConfigs[band];
        filter.set(cfg.freq, (float)cfg.width / 10, mSampleRate, dbGain);
        if (clearState) {
            filter.clearState();
        }
    }
    void setAllGains(const int8_t* gains)
    {
        if (gains) {
            for (int i = 0; i < kBandCount; i++) {
                auto& cfg = mBandConfigs[i];
                mFilters[i].set(cfg.freq, (float)cfg.width / 10, mSampleRate, gains[i]);
            }
        } else {
            for (int i = 0; i < kBandCount; i++) {
                setBandGain(i, 0, true);
            }
        }
    }
};
#endif // EQUALIZERNODE_HPP
