#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <array>

struct EqBandConfig {
    int freq;
    float width;
    static const std::array<EqBandConfig, 10> kPresetTenBand;
    static const std::array<EqBandConfig, 5> kPresetFiveBand;
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
        : mBandConfigs(cfg){}
    const EqBandConfig& bandConfig(int n) const { return mBandConfigs[n]; }
    const BiQuad<Sample>& filter(uint8_t band) const
    {
        bqassert(band < kBandCount);
        return mFilters[band];
    }
    void initBand(int n, BiQuadType type, float dbGain)
    {
        bqassert(n < kBandCount);
        auto& cfg = mBandConfigs[n];
        mFilters[n].init(type, cfg.freq, cfg.width, mSampleRate, dbGain);
    }
    void init(int sr, float* gains)
    {
        mSampleRate = sr;
        initBand(0, BiQuadType::LSH, gains ? gains[0] : 0);
        int last = kBandCount - 1;
        if (gains) {
            for (int i = 1; i < last; i++) {
                initBand(i, BiQuadType::PEQ, gains[i]);
            }
        } else {
            for (int i = 1; i < last; i++) {
                initBand(i, BiQuadType::PEQ, 0);
            }
        }
        initBand(last, BiQuadType::HSH, gains ? gains[last] : 0);
    }
    Equalizer::Sample process(Sample sample)
    {
        auto end = mFilters + kBandCount;
        for (auto filter = mFilters; filter < end; filter++) {
            sample = filter->process(sample);
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
        }
        return s;
    }
    void setBandGain(uint8_t band, float dbGain, bool clearState=false)
    {
        bqassert(band < kBandCount);
        auto& filter = mFilters[band];
        auto& cfg = mBandConfigs[band];
        filter.setup(cfg.freq, cfg.width, mSampleRate, dbGain);
        if (clearState) {
            filter.clearHistorySamples();
        }
    }
    void dumpAllGains(Sample *gains)
    {
        for (int i = 0; i < kBandCount; i++) {
            gains[i] = mFilters[i].dbGain();
        }
    }
    void zeroAllGains()
    {
        for (int i = 0; i < kBandCount; i++) {
            setBandGain(i, 0, true);
        }
    }
    float bandGain(uint8_t band) { return filter(band).dbGain(); }
};
#endif // EQUALIZERNODE_HPP
