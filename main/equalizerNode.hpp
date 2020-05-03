#ifndef EQUALIZERNODE_HPP
#define EQUALIZERNODE_HPP
#include "biquad.hpp"

class Equalizer
{
protected:
    BiQuad<double> mFilters[10];
    double mSampleRate;
public:
    Equalizer();
};

Equalizer::Equalizer(int sr)
:mSampleRate(sr)
{
    mFilters[0].init(BiQuad::LPF, 0, 31.5, sr, 1);
    mFilters[1].init(BiQuad::LPF, 0, 63.0, sr, 1);
    mFilters[2].init(BiQuad::LPF, 0, 125.0, sr, 1);
    mFilters[3].init(BiQuad::LPF, 0, 250.0, sr, 1);
    mFilters[4].init(BiQuad::LPF, 0, 500.0, sr, 1);
    mFilters[5].init(BiQuad::LPF, 0, 1000.0, sr, 1);
    mFilters[6].init(BiQuad::LPF, 0, 2000.0, sr, 1);
    mFilters[7].init(BiQuad::LPF, 0, 4000.0, sr, 1);
    mFilters[8].init(BiQuad::LPF, 0, 8000.0, sr, 1);
    mFilters[9].init(BiQuad::LPF, 0, 16000.0, sr, 1);
}
/*
 * Normally you calculate dB as dB = 10*log(output/input) for gain of a system as
 * an example. Another way to
 * calculate dB is dB = 10*log(measured_value/minimum_value)
 * where minimum_value is the minimum detectible amount,
 * so in a 16bit audio the minimum would be 1/32768.0f.
 */

class equalizerNode
{
public:
    equalizerNode();
};

#endif // EQUALIZERNODE_HPP
