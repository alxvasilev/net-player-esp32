#ifndef EQUALIZER_AV_HPP
#define EQUALIZER_AV_HPP
#include "biquad.hpp"
#include <limits>

class Equalizer
{
public:
    typedef double Sample;
    static const int bandFreqs[10];
protected:
    BiQuad<Sample> mFilters[10];
    Sample mSampleRate;
    uint8_t mHighestBand;
public:
    const BiQuad<Sample>& filter(uint8_t band) const
    {
        bqassert(band <= 9);
        return mFilters[band];
    }
    void init(int samplerate, Sample* gains=nullptr);
    Sample process(Sample sample);
    template<typename T>
    T processInt(T sample) {
        Sample s = process(sample);
        if (s > std::numeric_limits<T>::max()) {
            s = std::numeric_limits<T>::max();
        } else if (s < std::numeric_limits<T>::min()) {
            s = std::numeric_limits<T>::min();
        }
        return s;
    }
    void setBandGain(uint8_t band, Sample gain);
    Sample bandGain(uint8_t band) { return filter(band).dbGain(); }
    void dumpAllGains(Sample* gains);
};
#endif // EQUALIZERNODE_HPP
