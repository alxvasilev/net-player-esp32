#ifndef BIQUAD_HPP
#define BIQUAD_HPP

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
        kPeaking,
        kLowShelf,
        kHighShelf
    };
    typedef float Float; // floating-point type used for samples and calculations
protected:
    union {
        Float m_coeffs[5];
        struct {
            Float m_b0;
            Float m_b1;
            Float m_b2;
            Float m_a1;
            Float m_a2;
        };
    };
    Biquad() {}
public:
    /** Calculate coefficients */
    void recalc(Type type, uint16_t freq, float Q, uint32_t srate, int8_t dbGain)
    {
        if (type != kPeaking) {
            dbGain *= 2;
        }
        double A = pow(10.0, (float)dbGain / 40.0f);
        double w0 = 2 * M_PI * (double)freq / (double)srate;
        double sn = sin(w0);
        double cs = cos(w0);
        double alpha = sn / (2 * Q);
        if (type == kPeaking) {
            double a0inv = 1 / (1 + alpha / A);
            m_a1 = (-2 * cs) * a0inv;
            m_a2 = (1 - alpha / A) * a0inv;
            m_b0 = (1 + alpha * A) * a0inv;
            m_b1 = (-2 * cs) * a0inv;
            m_b2 = (1 - alpha * A) * a0inv;
            return;
        }
        double appm = (A + 1) + (A - 1) * cs;
        double apmm = (A + 1) - (A - 1) * cs;
        double ampp = (A - 1) + (A + 1) * cs;
        double ammp = (A - 1) - (A + 1)*cs;
        double betasn = sqrt(A) * sn / Q;
        // When Q = 0.707 (= 1/sqrt(2) = 1 octave), betasn transforms to sqrt(2 * A) * sn, i.e. beta * sn
        if (type == kLowShelf) {
            double a0inv = 1 / (appm + betasn);
            m_a1 = -2 * ampp * a0inv;
            m_a2 = (appm - betasn) * a0inv;
            m_b0 = A * (apmm + betasn) * a0inv;
            m_b1 = 2 * A * ammp * a0inv;
            m_b2 = A * (apmm - betasn) * a0inv;
        }
        else if (type == kHighShelf) {
            double a0inv = 1 / (apmm + betasn);
            m_a1 = 2 * ammp * a0inv;
            m_a2 = (apmm - betasn) * a0inv;
            m_b0 = A * (appm + betasn) * a0inv;
            m_b1 = -2 * A * ampp * a0inv;
            m_b2 = A * (appm - betasn) * a0inv;
        }
        else {
            bqassert(false);
        }
        BQ_LOGD("Config band %d Hz, Q: %f, gain: %f", freq, Q, dbGain);
        BQ_LOGD("coeffs: b0 = %f, b1 = %f, b2 = %f, a1 = %f, a2 = %f", b0, b1, b2, a1, a2);
    }
};

class BiquadMono: public Biquad {
protected:
    Float mDelay[2]; //delay line, for Direct Form 2
public:
    BiquadMono() { clearState(); }
    void clearState()
    {
        mDelay[0] = mDelay[1] = 0.0;
    }
    inline void process_asm(Float* samples, int len)
    {
        asmBiquad_f32_df2_mono(samples, samples, len, m_coeffs, mDelay);
    }
    void process(Float* samples, int len)
    {
        Float b0 = m_b0;
        Float b1 = m_b1;
        Float b2 = m_b2;
        Float a1 = m_a1;
        Float a2 = m_a2;
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
    BiquadStereo() { clearState(); }
    void clearState()
    {
        mDelayL[0] = mDelayL[1] = mDelayR[0] = mDelayR[1] = 0.0;
    }
    inline void process(Float* samples, int len)
    {
        asmBiquad_f32_df2_stereo(samples, len, m_coeffs, mDelayL, mDelayR);
    }
    void process_C(Float* samples, int len)
    {
        Float dlyL0 = mDelayL[0];
        Float dlyL1 = mDelayL[1];
        Float dlyR0 = mDelayR[0];
        Float dlyR1 = mDelayR[1];
        Float b0 = m_b0;
        Float b1 = m_b1;
        Float b2 = m_b2;
        Float a1 = m_a1;
        Float a2 = m_a2;
        Float* end = samples + 2 * len;
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
