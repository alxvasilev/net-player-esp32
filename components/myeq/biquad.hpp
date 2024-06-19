#ifndef BIQUAD_HPP
#define BIQUAD_HPP
/* Simple implementation of Biquad filters -- Tom St Denis
*
* Based on the work

Cookbook formulae for audio EQ biquad filter coefficients
---------------------------------------------------------
by Robert Bristow-Johnson, pbjrbj@viconet.com  a.k.a. robert@audioheads.com

* Available on the web at

http://www.smartelectronix.com/musicdsp/text/filters005.txt

* Enjoy.
*
* This work is hereby placed in the public domain for all purposes, whether
* commercial, free [as in speech] or educational, etc.  Use the code and please
* give me credit if you wish.
*
* Tom St Denis -- http://tomstdenis.home.dhs.org*/

#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef BQ_DEBUG
    #define bqassert(cond) \
        if (!(cond)) { fprintf(stderr, "Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); }
    #define BQ_LOGD(fmt,...) fprintf(stderr, fmt "\n", ##__VA_ARGS__)
#else
    #define bqassert(cond)
    #define BQ_LOGD(fmt,...)
#endif

extern "C" int asmBiquad_f32_df2_mono(const float* input, float* output, int len, float* coefs, float* delays);
extern "C" int asmBiquad_f32_df2_stereo(const float* samples, int len, float* coefs,
    float* delaysL, float* delaysR);

class Biquad
{
public:
    enum Type: uint8_t {
        kBand, // Peaking (positive gain) or Notch (negative gain) filter
        kLowShelf,
        kHighShelf
    };
    typedef float Float; // floating-point type used for samples and calculations
protected:
    Type mType;
    Float mCoeffs[5];
    Biquad() {}
public:
    Type type() const { return mType; }
    /** Reconfigure the filter, usually used for adjusting the gain during operation */
    void set(int freq, float Q, int srate, float dbGain)
    {
        double a0inv, a1, a2, b0, b1, b2;
        double V = powf(10.0f, fabs(dbGain) / 20.0f);
        double K = tan(M_PI * freq / srate);

        switch (mType) {
        case kBand:
            if (dbGain >= 0) {
                a0inv = 1 / (1 + 1/Q * K + K * K);
                b0 = (1 + V/Q * K + K * K) * a0inv;
                b1 = 2 * (K * K - 1) * a0inv;
                b2 = (1 - V/Q * K + K * K) * a0inv;
                a1 = b1;
                a2 = (1 - 1/Q * K + K * K) * a0inv;
            }
            else {
                a0inv = 1 / (1 + K / Q + K * K);
                b0 = (1 + K * K) * a0inv;
                b1 = 2 * (K * K - 1) * a0inv;
                b2 = b0;
                a1 = b1;
                a2 = (1 - K / Q + K * K) * a0inv;
            }
            break;
        case kLowShelf:
            if (dbGain >= 0) {
                a0inv = 1 / (1 + sqrt(2) * K + K * K);
                b0 = (1 + sqrt(2*V) * K + V * K * K) * a0inv;
                b1 = 2 * (V * K * K - 1) * a0inv;
                b2 = (1 - sqrt(2*V) * K + V * K * K) * a0inv;
                a1 = 2 * (K * K - 1) * a0inv;
                a2 = (1 - sqrt(2) * K + K * K) * a0inv;
            }
            else {
                a0inv = 1 / (1 + sqrt(2*V) * K + V * K * K);
                b0 = (1 + sqrt(2) * K + K * K) * a0inv;
                b1 = 2 * (K * K - 1) * a0inv;
                b2 = (1 - sqrt(2) * K + K * K) * a0inv;
                a1 = 2 * (V * K * K - 1) * a0inv;
                a2 = (1 - sqrt(2*V) * K + V * K * K) * a0inv;
            }
            break;
        case kHighShelf:
            if (dbGain >= 0) {
                a0inv = 1 / (1 + sqrt(2) * K + K * K);
                b0 = (V + sqrt(2*V) * K + K * K) * a0inv;
                b1 = 2 * (K * K - V) * a0inv;
                b2 = (V - sqrt(2*V) * K + K * K) * a0inv;
                a1 = 2 * (K * K - 1) * a0inv;
                a2 = (1 - sqrt(2) * K + K * K) * a0inv;
            }
            else {
                a0inv = 1 / (V + sqrt(2*V) * K + K * K);
                b0 = (1 + sqrt(2) * K + K * K) * a0inv;
                b1 = 2 * (K * K - 1) * a0inv;
                b2 = (1 - sqrt(2) * K + K * K) * a0inv;
                a1 = 2 * (K * K - V) * a0inv;
                a2 = (V - sqrt(2*V) * K + K * K) * a0inv;
            }
            break;
        default:
            b0 = b1 = b2 = a0inv = a1 = a2 = 0; //suppress may be used uninitialized warning
            bqassert(false);
        }
        mCoeffs[0] = b0;
        mCoeffs[1] = b1;
        mCoeffs[2] = b2;
        mCoeffs[3] = a1;
        mCoeffs[4] = a2;
        BQ_LOGD("Config band %d Hz, Q: %f, gain: %f", freq, Q, dbGain);
        BQ_LOGD("coeffs: b0 = %f, b1 = %f, b2 = %f, a1 = %f, a2 = %f",
                mCoeffs[0], mCoeffs[1], mCoeffs[2], mCoeffs[3], mCoeffs[4]);
    }
};

class BiquadMono: public Biquad {
protected:
    Float mDelay[2]; //delay line, for Direct Form 2
public:
    void clearState()
    {
        mDelay[0] = mDelay[1] = 0.0;
    }
    void init(Type type)
    {
        mType = type;
        clearState();
    }
    inline void process_asm(Float* samples, int len)
    {
        asmBiquad_f32_df2_mono(samples, samples, len, mCoeffs, mDelay);
    }
    void process(Float* samples, int len)
    {
        auto b0 = mCoeffs[0];
        auto b1 = mCoeffs[1];
        auto b2 = mCoeffs[2];
        auto a1 = mCoeffs[3];
        auto a2 = mCoeffs[4];
        Float dly0 = mDelay[0];
        Float dly1 = mDelay[1];
        Float* end = samples + len;
        for (; samples < end; samples++) {
            Float in = *samples;
            Float out = in * b0 + dly0;
            dly0 = in * b1 + dly1 - a1 * out;
            dly1 = in * b2 - a2 * out;
            *samples = out;
        }
        mDelay[0] = dly0;
        mDelay[1] = dly1;
    }
};

class BiquadStereo: public Biquad {
protected:
    Float mDelayL[2]; //delay line, for Direct Form 2
    Float mDelayR[2];
public:
    void clearState()
    {
        mDelayL[0] = mDelayL[1] = mDelayR[0] = mDelayR[1] = 0.0;
    }
    void init(Type type)
    {
        mType = type;
        clearState();
    }
    inline void process(Float* samples, int len)
    {
        asmBiquad_f32_df2_stereo(samples, len, mCoeffs, mDelayL, mDelayR);
    }
    void process_C(Float* samples, int len)
    {
        Float dlyL0 = mDelayL[0];
        Float dlyL1 = mDelayL[1];
        Float dlyR0 = mDelayR[0];
        Float dlyR1 = mDelayR[1];
        auto b0 = mCoeffs[0];
        auto b1 = mCoeffs[1];
        auto b2 = mCoeffs[2];
        auto a1 = mCoeffs[3];
        auto a2 = mCoeffs[4];
        auto end = samples + 2 * len;
        while(samples < end) {
            Float in = *samples;
            Float out = in * b0 + dlyL0;
            dlyL0 = in * b1 + dlyL1 - a1 * out;
            dlyL1 = in * b2 - a2 * out;
            *(samples++) = out;

            in = *samples;
            out = in * b0 + dlyR0;
            dlyR0 = in * b1 + dlyR1 - a1 * out;
            dlyR1 = in * b2 - a2 * out;
            *(samples++) = out;
        }
        mDelayL[0] = dlyL0;
        mDelayL[1] = dlyL1;
        mDelayR[0] = dlyR0;
        mDelayR[1] = dlyR1;
    }
};

#endif
