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
extern "C" int asmBiquad_f32_df2_stereo(const float* input, float* output, int len, float* coefs,
    float* delaysL, float* delaysR);

class Biquad
{
public:
    enum Type: uint8_t {
        LPF, /* low pass filter */
        HPF, /* High pass filter */
        BPF, /* band pass filter */
        NOTCH, /* Notch Filter */
        PEQ, /* Peaking band EQ filter */
        LSH, /* Low shelf filter */
        HSH /* High shelf filter */
    };
    typedef float Float; // floating-point type used for samples and calculations
protected:
    Type mType;
    Float mCoeffs[5];
    Biquad() {}
public:
    Type type() const { return mType; }
    bool usesGain()
    {
        return (mType == HSH || mType == LSH || mType == PEQ);
    }
    /** Reconfigure the filter, usually used for adjusting the gain during operation */
    void set(int freq, float bw, int srate, float dbGain)
    {
        double a0, a1, a2, b0, b1, b2;
        /* setup variables */
        double A = usesGain() ? powf(10.0f, dbGain / 20.0f) : 0;
        double omega = 2.0 * M_PI * freq / srate;
        double sn = sin(omega);
        double cs = cos(omega);
        double alpha = sn * sinh(M_LN2 / 2.0 * bw * omega /sn);
        double beta = sqrt(A + A);

        switch (mType) {
        case LPF:
            b0 = (1.0f - cs) / 2.0f;
            b1 = 1.0f - cs;
            b2 = (1.0f - cs) / 2.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha;
            break;
        case HPF:
            b0 = (1 + cs) /2;
            b1 = -(1 + cs);
            b2 = (1 + cs) /2;
            a0 = 1 + alpha;
            a1 = -2 * cs;
            a2 = 1 - alpha;
            break;
        case BPF:
            b0 = alpha;
            b1 = 0;
            b2 = -alpha;
            a0 = 1 + alpha;
            a1 = -2 * cs;
            a2 = 1 - alpha;
            break;
        case NOTCH:
            b0 = 1;
            b1 = -2 * cs;
            b2 = 1;
            a0 = 1 + alpha;
            a1 = -2 * cs;
            a2 = 1 - alpha;
            break;
        case PEQ:
            b0 = 1.0 + (alpha * A);
            b1 = -2.0 * cs;
            b2 = 1.0 - (alpha * A);
            a0 = 1.0 + (alpha / A);
            a1 = -2.0 * cs;
            a2 = 1.0 - (alpha / A);
            break;
        case LSH:
            b0 = A * ((A + 1) - (A - 1) * cs + beta * sn);
            b1 = 2 * A * ((A - 1) - (A + 1) * cs);
            b2 = A * ((A + 1) - (A - 1) * cs - beta * sn);
            a0 = (A + 1) + (A - 1) * cs + beta * sn;
            a1 = -2 * ((A - 1) + (A + 1) * cs);
            a2 = (A + 1) + (A - 1) * cs - beta * sn;
            break;
        case HSH:
            b0 = A * ((A + 1) + (A - 1) * cs + beta * sn);
            b1 = -2 * A * ((A - 1) + (A + 1) * cs);
            b2 = A * ((A + 1) + (A - 1) * cs - beta * sn);
            a0 = (A + 1) - (A - 1) * cs + beta * sn;
            a1 = 2 * ((A - 1) - (A + 1) * cs);
            a2 = (A + 1) - (A - 1) * cs - beta * sn;
            break;
        default:
            b0 = b1 = b2 = a0 = a1 = a2 = 0; //suppress may be used uninitialized warning
            bqassert(false);
        }
        /* precompute the coefficients */
        mCoeffs[0] = b0 / a0;
        mCoeffs[1] = b1 / a0;
        mCoeffs[2] = b2 / a0;
        mCoeffs[3] = a1 / a0;
        mCoeffs[4] = a2 / a0;
        BQ_LOGD("Config band %d Hz, bw: %f, gain: %f", freq, bw, dbGain);
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
    inline void process(Float* samples, int len)
    {
        asmBiquad_f32_df2_mono(samples, samples, len, mCoeffs, mDelay);
    }
    void process_C(Float* samples, int len)
    {
        Float dly0 = mDelay[0];
        Float dly1 = mDelay[1];
        for (int i = 0; i < len; i++) {
            Float d0 = samples[i] - mCoeffs[3] * dly0 - mCoeffs[4] * dly1;
            samples[i] = mCoeffs[0] * d0 +  mCoeffs[1] * dly0 + mCoeffs[2] * dly1;
            dly1 = dly0;
            dly0 = d0;
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
        //printf("asm process %d\n", len);
        //asmBiquad_f32_df2_stereo(samples, samples, len, mCoeffs, mDelayL, mDelayR);
        asmBiquad_f32_df2_mono(samples, samples, len * 2, mCoeffs, mDelayL);
        //printf("asm process done\n");
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
        for (int i = 0; i < len; i++) {
            Float d0 = *samples - a1 * dlyL0 - a2 * dlyL1;
            *(samples++) = b0 * d0 +  b1 * dlyL0 + b2 * dlyL1;
            dlyL1 = dlyL0;
            dlyL0 = d0;

            d0 = *samples - a1 * dlyR0 - a2 * dlyR1;
            *(samples++) = b0 * d0 +  b1 * dlyR0 + b2 * dlyR1;
            dlyR1 = dlyR0;
            dlyR0 = d0;
        }
        mDelayL[0] = dlyL0;
        mDelayL[1] = dlyL1;
        mDelayR[0] = dlyR0;
        mDelayR[1] = dlyR1;
    }
};

#endif
