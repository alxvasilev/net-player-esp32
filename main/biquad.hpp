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

#define bqassert(cond) \
    if (!(cond)) { printf("Assertion failed: %s at %s:%d\n", #cond, __FILE__, __LINE__); }

enum BiQuadType: uint8_t {
    LPF, /* low pass filter */
    HPF, /* High pass filter */
    BPF, /* band pass filter */
    NOTCH, /* Notch Filter */
    PEQ, /* Peaking band EQ filter */
    LSH, /* Low shelf filter */
    HSH /* High shelf filter */
};

template <typename S>
class BiQuad
{
public:
    typedef S Sample;
protected:
    typedef float Float; // floating-point type for coefficient calculation
    typedef typename std::conditional<sizeof(S) <= 2, uint32_t, uint64_t>::type WideInt;
    BiQuadType m_type;
    /* filter types */
    Sample m_a0, m_a1, m_a2, m_a3, m_a4;
    Sample m_x1, m_x2, m_y1, m_y2;
    float m_dbGain;
public:
    BiQuadType type() const { return m_type; }
    bool usesGain()
    {
        return (m_type == BiQuadType::HSH || m_type == BiQuadType::LSH ||
                m_type == BiQuadType::PEQ);
    }
    /** Process one sample */
    Sample process(Sample sample)
    {
        Sample result;

        /* compute result */
        result = m_a0 * sample + m_a1 * m_x1 + m_a2 * m_x2 -
                m_a3 * m_y1 - m_a4 * m_y2;

        /* shift x1 to x2, sample to x1 */
        m_x2 = m_x1;
        m_x1 = sample;

        /* shift y1 to y2, result to y1 */
        m_y2 = m_y1;
        m_y1 = result;

        return result;
    }
    void init(BiQuadType type,         /* type of filter */
              Sample dbGain,           /* gain of filter in dB */
              Sample freq,             /* center frequency */
              Sample srate,            /* sampling rate */
              Sample bandwidth)        /* bandwidth in octaves */
    {
        m_type = type;
        setup(freq, bandwidth, dbGain, srate);
        clearHistorySamples();
    }
    double dbGain() const { return m_dbGain; }
    void setup(Sample freq, Sample bw, Float dbGain, Sample srate)
    {
        m_dbGain = dbGain;
        Float a0, a1, a2, b0, b1, b2;

        /* setup variables */
        float A = usesGain() ? powf(10.0f, dbGain / 20.0f) : 0;
        Float omega = 2.0 * M_PI * freq / srate;
        Float sn = sin(omega);
        Float cs = cos(omega);
        Float alpha = sn * sinh(M_LN2 / 2.0 * bw * omega /sn);
        Float beta = sqrt(A + A);

        switch (m_type) {
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
            b0=b1=b2=a0=a1=a2=0; //suppress may be used uninitialized warning
            bqassert(false);
        }

        /* precompute the coefficients */
        m_a0 = b0 / a0;
        m_a1 = b1 / a0;
        m_a2 = b2 / a0;
        m_a3 = a1 / a0;
        m_a4 = a2 / a0;
        printf("a0=%f, a1=%f, a2=%f, a3=%f, a4=%f\n", m_a0, m_a1, m_a2, m_a3, m_a4);
    }
    void clearHistorySamples()
    {
        /* zero initial samples */
        m_x1 = m_x2 = 0;
        m_y1 = m_y2 = 0;
    }
};
#endif
