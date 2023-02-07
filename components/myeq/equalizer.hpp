#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <array>

struct EqBandConfig {
    int freq;
    float width;
    static const std::array<EqBandConfig, 10> kPreset10Band;
    static const std::array<EqBandConfig, 8> kPreset8Band;
    static const std::array<EqBandConfig, 6> kPreset6Band;
    static const std::array<EqBandConfig, 5> kPreset5Band;
};

template <int N, typename S>
class Equalizer
{
public:
    typedef S Sample;
    enum { kBandCount = N };
protected:
    const std::array<EqBandConfig, kBandCount>& mBandConfigs;
    BiQuad<Sample> mFilters[kBandCount];
    int mSampleRate;
public:
    Equalizer(const std::array<EqBandConfig, kBandCount>& cfg)
        : mBandConfigs(cfg)
    {
        mFilters[0].init(BiQuadType::LSH);
        mFilters[kBandCount - 1].init(BiQuadType::HSH);
        for (int i = 1; i < kBandCount - 1; i++) {
            mFilters[i].init(BiQuadType::PEQ);
        }
    }
    const EqBandConfig& bandConfig(int n) const { return mBandConfigs[n]; }
    const BiQuad<Sample>& filter(uint8_t band) const
    {
        bqassert(band < kBandCount);
        return mFilters[band];
    }
    void init(int sr, float* gains)
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
    void setBandGain(uint8_t band, float dbGain, bool clearState=false)
    {
        bqassert(band < kBandCount);
        auto& filter = mFilters[band];
        auto& cfg = mBandConfigs[band];
        filter.set(cfg.freq, cfg.width, mSampleRate, dbGain);
        if (clearState) {
            filter.clearHistorySamples();
        }
    }
    void setAllGains(float* gains)
    {
        for (int i = 0; i < kBandCount; i++) {
            auto& cfg = mBandConfigs[i];
            mFilters[i].set(cfg.freq, cfg.width, mSampleRate, gains[i]);
        }
    }
    void zeroAllGains()
    {
        for (int i = 0; i < kBandCount; i++) {
            setBandGain(i, 0, true);
        }
    }
};
#endif // EQUALIZERNODE_HPP
