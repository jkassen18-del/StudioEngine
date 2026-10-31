#pragma once
// Studio Engine DSP core - RBJ biquad filters (no external dependencies).
#include <algorithm>
#include <cmath>
#include <complex>

namespace se
{
constexpr double kPi = 3.14159265358979323846;

inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double g, double floorDb = -150.0)
{
    return g > 0.0 ? std::max (floorDb, 20.0 * std::log10 (g)) : floorDb;
}

enum class FilterType
{
    LowPass,
    HighPass,
    BandPass,
    Peak,
    LowShelf,
    HighShelf
};

struct BiquadCoeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    /** Magnitude response in dB at frequency f. */
    double magnitudeDb (double f, double fs) const
    {
        const double w = 2.0 * kPi * f / fs;
        const std::complex<double> z1 = std::polar (1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const auto num = b0 + b1 * z1 + b2 * z2;
        const auto den = 1.0 + a1 * z1 + a2 * z2;
        return gainToDb (std::abs (num / den));
    }
};

/** Robert Bristow-Johnson cookbook coefficients. For shelves Q acts as the
    shelf "Q" (0.7071 == slope S of 1). */
inline BiquadCoeffs makeBiquad (FilterType type, double fs, double f0, double Q, double gainDb = 0.0)
{
    f0 = std::clamp (f0, 1.0, fs * 0.49);
    Q = std::max (Q, 0.025);
    const double A = std::pow (10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * f0 / fs;
    const double cw = std::cos (w0);
    const double sw = std::sin (w0);
    const double alpha = sw / (2.0 * Q);

    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

    switch (type)
    {
        case FilterType::LowPass:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case FilterType::HighPass:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case FilterType::BandPass: // constant 0 dB peak gain
            b0 = alpha; b1 = 0; b2 = -alpha;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case FilterType::Peak:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case FilterType::LowShelf:
        {
            const double sA = 2 * std::sqrt (A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + sA);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sA);
            a0 = (A + 1) + (A - 1) * cw + sA;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sA;
            break;
        }
        case FilterType::HighShelf:
        {
            const double sA = 2 * std::sqrt (A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + sA);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sA);
            a0 = (A + 1) - (A - 1) * cw + sA;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sA;
            break;
        }
    }

    BiquadCoeffs c;
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0;
    c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

/** Transposed direct form II biquad with double precision state. */
struct Biquad
{
    BiquadCoeffs c;
    double z1 = 0.0, z2 = 0.0;

    void reset() { z1 = z2 = 0.0; }
    void set (const BiquadCoeffs& coeffs) { c = coeffs; }

    inline float process (float xf)
    {
        const double x = xf;
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

/** One-pole smoothing coefficient for a time constant in milliseconds. */
inline double timeCoeff (double ms, double fs)
{
    if (ms <= 0.0)
        return 0.0;
    return std::exp (-1.0 / (0.001 * ms * fs));
}

} // namespace se
