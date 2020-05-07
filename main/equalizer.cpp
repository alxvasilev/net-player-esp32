#include "equalizer.hpp"

const int Equalizer::bandFreqs[10] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

void Equalizer::init(int sr, Sample* gains)
{
    static Sample zeroGains[10] = {0,0,0,0,0,0,0,0,0,0};
    if (!gains) {
        gains = zeroGains;
    }
    mSampleRate = sr;
    mFilters[0].init(BiQuadType::LPF, gains[0], bandFreqs[0], sr, 1);
    mFilters[1].init(BiQuadType::LPF, gains[1], bandFreqs[1], sr, 0.8);
    mFilters[2].init(BiQuadType::LPF, gains[2], bandFreqs[2], sr, 0.8);
    mFilters[3].init(BiQuadType::LPF, gains[3], bandFreqs[3], sr, 0.8);
    mFilters[4].init(BiQuadType::LPF, gains[4], bandFreqs[4], sr, 0.8);
    mFilters[5].init(BiQuadType::LPF, gains[5], bandFreqs[5], sr, 0.8);
    mFilters[6].init(BiQuadType::LPF, gains[6], bandFreqs[6], sr, 0.8);
    mFilters[7].init(BiQuadType::LPF, gains[7], bandFreqs[7], sr, 0.5);
    mFilters[8].init(BiQuadType::LPF, gains[8], bandFreqs[8], sr, 0.5);
    if (sr >= 23000) {
        mFilters[9].init(BiQuadType::LPF, gains[9], bandFreqs[9], sr, 1);
        mHighestBand = 9;
    } else {
        mHighestBand = 8;
    }
}
Equalizer::Sample Equalizer::process(Sample in)
{
    Sample out = 0;
    for (int i = 0; i <= mHighestBand; i++) {
        auto& filter = mFilters[i];
        out += filter.process(in) * filter.gainMultiplier();
    }
    return out / (mHighestBand + 1);
}

void Equalizer::setBandGain(uint8_t band, Sample dbGain)
{
    if (band > 9) {
        return;
    }
    auto& filter = mFilters[band];
    if (filter.hasOwnGain()) {
        filter.recalcCoeffs(bandFreqs[band], 1, dbGain, mSampleRate);
    } else {
        filter.setGainNoRecalc(dbGain);
    }
}

void Equalizer::dumpAllGains(Sample *gains)
{
    for (int i = 0; i<10; i++) {
        gains[i] = mFilters[i].dbGain();
    }
}
