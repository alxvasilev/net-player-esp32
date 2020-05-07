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

template <class S>
class BiQuad
{
public:
/* whatever sample type you want */
    typedef S Sample;
protected:
/* filter types */
    BiQuadType mType;
    Sample ma0, ma1, ma2, ma3, ma4;
    Sample mx1, mx2, my1, my2;
    Sample mA;
public:
    BiQuadType type() const { return mType; }
/* Computes a BiQuad filter on a sample */
    Sample process(Sample sample)
    {
        Sample result;

        /* compute result */
        result = ma0 * sample + ma1 * mx1 + ma2 * mx2 -
                ma3 * my1 - ma4 * my2;

        /* shift x1 to x2, sample to x1 */
        mx2 = mx1;
        mx1 = sample;

        /* shift y1 to y2, result to y1 */
        my2 = my1;
        my1 = result;

        return result;
    }
    void init(BiQuadType type,
              Sample dbGain,           /* gain of filter in dB */
              Sample freq,             /* center frequency */
              Sample srate,            /* sampling rate */
              Sample bandwidth)        /* bandwidth in octaves */
    {
        mType = type;
        recalcCoeffs(freq, bandwidth, dbGain, srate);
    }
    bool hasOwnGain() const
    {
        return (mType == BiQuadType::HSH || mType == BiQuadType::LSH ||
                mType == BiQuadType::PEQ);
    }
    void setGainNoRecalc(Sample dbGain)
    {
        bqassert(!hasOwnGain());
        mA = pow(10, dbGain / 20);
    }
    double gainMultiplier() const { return mA; }
    double dbGain() const { return log10(mA) * 20; }
    void recalcCoeffs(Sample freq, Sample bw, Sample gain, Sample srate)
    {
        Sample omega, sn, cs, alpha, beta;
        Sample a0, a1, a2, b0, b1, b2;

        /* setup variables */
        mA = pow(10, gain / 20);
        omega = 2 * M_PI * freq / srate;
        sn = sin(omega);
        cs = cos(omega);
        alpha = sn * sinh(M_LN2 /2 * bw * omega /sn);
        beta = sqrt(mA + mA);

        switch (mType) {
        case LPF:
            b0 = (1 - cs) /2;
            b1 = 1 - cs;
            b2 = (1 - cs) /2;
            a0 = 1 + alpha;
            a1 = -2 * cs;
            a2 = 1 - alpha;
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
            b0 = 1 + (alpha * mA);
            b1 = -2 * cs;
            b2 = 1 - (alpha * mA);
            a0 = 1 + (alpha /mA);
            a1 = -2 * cs;
            a2 = 1 - (alpha /mA);
            break;
        case LSH:
            b0 = mA * ((mA + 1) - (mA - 1) * cs + beta * sn);
            b1 = 2 * mA * ((mA - 1) - (mA + 1) * cs);
            b2 = mA * ((mA + 1) - (mA - 1) * cs - beta * sn);
            a0 = (mA + 1) + (mA - 1) * cs + beta * sn;
            a1 = -2 * ((mA - 1) + (mA + 1) * cs);
            a2 = (mA + 1) + (mA - 1) * cs - beta * sn;
            break;
        case HSH:
            b0 = mA * ((mA + 1) + (mA - 1) * cs + beta * sn);
            b1 = -2 * mA * ((mA - 1) + (mA + 1) * cs);
            b2 = mA * ((mA + 1) + (mA - 1) * cs - beta * sn);
            a0 = (mA + 1) - (mA - 1) * cs + beta * sn;
            a1 = 2 * ((mA - 1) - (mA + 1) * cs);
            a2 = (mA + 1) - (mA - 1) * cs - beta * sn;
            break;
        default:
            b0=b1=b2=a0=a1=a2=0; //suppress may be used uninitialized warning
            bqassert(false);
        }

        /* precompute the coefficients */
        ma0 = b0 /a0;
        ma1 = b1 /a0;
        ma2 = b2 /a0;
        ma3 = a1 /a0;
        ma4 = a2 /a0;

        /* zero initial samples */
        mx1 = mx2 = 0;
        my1 = my2 = 0;
    }
};
#endif
