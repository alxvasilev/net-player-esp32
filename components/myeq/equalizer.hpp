#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <string.h>

struct EqBandConfig {
    uint16_t freq;
    uint16_t Q; // Q multiplied by 1000
    static const EqBandConfig kPreset10Band[10];
    static const EqBandConfig kPreset9Band[9];
    static const EqBandConfig kPreset8Band[8];
    static const EqBandConfig kPreset7Band[7];
    static const EqBandConfig kPreset6Band[6];
    static const EqBandConfig kPreset5Band[5];
    static const EqBandConfig kPreset4Band[4];
    static const EqBandConfig kPreset3Band[3];
    static const EqBandConfig* kBandPresets[8];
    static const EqBandConfig* defaultCfg(int n) {
        return (n < 3 || n > 10) ? nullptr : kBandPresets[n - 3];
    }
};

static_assert(sizeof(EqBandConfig) == 4, "");
static_assert(sizeof(BiquadStereo) % 4 == 0, "");
static_assert(sizeof(BiquadMono) % 4 == 0, "");

template <bool IsStereo>
class Equalizer
{
public:
    typedef typename std::conditional<IsStereo, BiquadStereo, BiquadMono>::type BiquadType;
    typedef int8_t Gain;
protected:
    uint8_t mBandCount;
    uint32_t mSampleRate;
    BiquadType* mFilters;
    // Need to have filter configs and gains in continguous arrays instead of members of each filter,
    // because it's simpler and more efficient to load and store them in NVS
    EqBandConfig* mBandConfigs;
    Gain* mGains;
    alignas (BiquadType) uint8_t mMem[];
    Equalizer(uint8_t nBands, uint32_t sampleRate):
        mBandCount(nBands), mSampleRate(sampleRate),
        mFilters((BiquadType*)mMem),
        mBandConfigs((EqBandConfig*)(mMem + sizeof(BiquadType) * nBands)),
        mGains((int8_t*)mBandConfigs + sizeof(EqBandConfig) * nBands)
    {
        memset(mGains, 0, nBands);
        memset(mBandConfigs, 0, nBands * sizeof(EqBandConfig));
        for (int i = 0; i < nBands; i++) {
            new (mFilters + i * sizeof(BiquadType)) BiquadType;
        }
    }
public:
    uint32_t numBands() const { return mBandCount; }
    uint32_t sampleRate() const { return mSampleRate; }
    EqBandConfig* bandConfigs() { return mBandConfigs; }
    EqBandConfig& bandConfig(uint8_t band) { return mBandConfigs[band]; }
    Gain* gains() { return mGains; }
    void setSampleRate(uint32_t sampleRate) { mSampleRate = sampleRate; updateAllFilters(); }
    // This class is not constructed directly via the ctor, but instead with a ::create factory function,
    // since additional runtime-determined amount of memory needs to be allocated at the end,
    // for the Biquad filters and the band configs
    static uint32_t instSize(uint8_t nBands) {
        return sizeof(Equalizer<IsStereo>) + nBands * (sizeof(BiquadType) + sizeof(EqBandConfig) + sizeof(Gain));
    }
    static Equalizer* create(uint8_t nBands, uint16_t sampleRate) {
        void* mem = malloc(instSize(nBands));
        if (!mem) {
            return nullptr;
        }
        return new(mem) Equalizer(nBands, sampleRate);
    }
    static Equalizer* create(uint8_t nBands, uint32_t sampleRate, void* placementMem) {
        bqassert(((uintptr_t)placementMem & 0x3) == 0); // must be 4-byte aligned
        return new(placementMem) Equalizer(nBands, sampleRate);
    }
    static void operator delete(void* p) { free(p); }
    void resetState()
    {
        for (int i = 0; i < mBandCount; i++) {
            mFilters[i].clearState();
        }
    }
    void process(float* input, int len)
    {
        for (int i = 0; i < mBandCount; i++) {
            mFilters[i].process(input, len);
        }
    }
    Biquad::Type filterTypeOfBand(uint8_t band) const {
        return band == 0 ? Biquad::kLowShelf : ((band == mBandCount - 1) ? Biquad::kHighShelf : Biquad::kPeaking);
    }
    void updateFilter(uint8_t band, bool clearState)
    {
        bqassert(band < mBandCount);
        auto& filter = mFilters[band];
        auto& cfg = mBandConfigs[band];
        // recalc(Type type, uint16_t freq, float Q, uint32_t srate, int8_t dbGain)
        filter.recalc(filterTypeOfBand(band), cfg.freq, (float)cfg.Q / 1000, mSampleRate, mGains[band]);
        if (clearState) {
            filter.clearState();
        }
    }
    void updateAllFilters(bool clearState)
    {
        for (int i = 0; i < mBandCount; i++) {
            updateFilter(i, clearState);
        }
    }
    void setBandGain(uint8_t band, Gain gain) {
        mGains[band] = gain;
        updateFilter(band, false);
    }
    void zeroAllGains(bool clearState=true) {
        memset(mGains, 0, mBandCount * sizeof(Gain));
        updateAllFilters(clearState);
    }
};
#endif // EQUALIZERNODE_HPP
