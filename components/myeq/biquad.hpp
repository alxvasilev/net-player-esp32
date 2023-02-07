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

enum BiQuadType: uint8_t {
    LPF, /* low pass filter */
    HPF, /* High pass filter */
    BPF, /* band pass filter */
    NOTCH, /* Notch Filter */
    PEQ, /* Peaking band EQ filter */
    LSH, /* Low shelf filter */
    HSH /* High shelf filter */
};

template <typename S, bool IsInt=std::is_integral<S>::value>
struct MulTraitsFor;

template<typename S>
struct MulTraitsFor<S, false>
{
    enum { kIsFloat = true };
    typedef S Sample;
    typedef S Wide;
    static Wide prepareCoeff(const double& coeff, const double& a0)
    {
        return coeff / a0;
    }
    static Sample normalizeResult(Wide result)
    {
        return (Sample)result;
    }
};

template<typename S>
struct MulTraitsFor<S, true>
{
    enum { kIsFloat = false };
    typedef S Sample;
    enum: bool { kIs32bit = sizeof(S) > 2 };
    typedef typename std::conditional<kIs32bit, int64_t, int32_t>::type Wide;
    enum { kCoefDecimalBits = kIs32bit ? 30 : 12 }; // 18 is the minimum for stable operation
    enum { kCoefDecimalMul = 1 << kCoefDecimalBits };
    static Wide prepareCoeff(double coeff, double a0)
    {
        return round(coeff * kCoefDecimalMul / a0);
    }
    static Sample normalizeResult(Wide result)
    {
        return (result + kCoefDecimalMul / 2 ) >> kCoefDecimalBits;
    }
};

template <typename S>
class BiQuad
{
public:
    typedef S Sample;
protected:
    typedef float Float; // floating-point type for coefficient calculation
    typedef MulTraitsFor<Sample> Mul;
    typedef typename Mul::Wide Wide;
    BiQuadType m_type;
    /* filter types */
    Wide m_a0, m_a1, m_a2, m_a3, m_a4;
    Sample m_x1, m_x2, m_y1, m_y2;
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
        /* compute result */
        Wide result = m_a0 * sample;
        result += m_a1 * m_x1;
        result += m_a2 * m_x2;
        result += m_a3 * m_y1;
        result += m_a4 * m_y2;
        /* shift x1 to x2, sample to x1 */
        m_x2 = m_x1;
        m_x1 = sample;
        /* shift y1 to y2, result to y1 */
        m_y2 = m_y1;
        m_y1 = Mul::normalizeResult(result);
        return m_y1;
    }
    /** Initialize the  biquad filter:
     *  @param type The type of filter
     *  @param freq The center/cutoff frequency, depending on filter type
     *  @param bw The bandwidth of the filter (if applicable), in octaves
     *  @param srate The sample rate of the stream, in Hz
     *  @param dbGain The gain of the filter, in dB
     */
    void init(BiQuadType type)
    {
        m_type = type;
        clearHistorySamples();
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
        m_a0 = Mul::prepareCoeff(b0, a0);
        m_a1 = Mul::prepareCoeff(b1, a0);
        m_a2 = Mul::prepareCoeff(b2, a0);
        m_a3 = -Mul::prepareCoeff(a1, a0);
        m_a4 = -Mul::prepareCoeff(a2, a0);
        BQ_LOGD("Config band %d Hz, bw: %f, gain: %f (%s)", freq, bw, dbGain, Mul::kIsFloat ? "fp" : "int");
        BQ_LOGD("a0=%f, a1=%f, a2=%f, a3=%f, a4=%f", b0/a0, b1/a0, b2/a0, a1/a0, a2/a0);
    }
    void clearHistorySamples()
    {
        /* zero initial samples */
        m_x1 = m_x2 = 0;
        m_y1 = m_y2 = 0;
    }
};
#endif
