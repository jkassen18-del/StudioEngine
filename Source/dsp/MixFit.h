#pragma once
// Studio Engine - "Mix Fit": pairs the vocal to the instrumental fed into the
// plugin's side-chain input.
//
//  1. Vocal rider - keeps the vocal's short-term loudness a fixed number of dB
//     relative to the instrumental (the "offset"), riding up to +/- range dB.
//  2. Unmasking   - when the beat is dense in the 1.5-5 kHz intelligibility
//     region, a dynamic presence boost lifts the vocal through it.
//
// With no side-chain signal present both stages stay neutral.
#include "Biquad.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace se
{
/** ITU-R BS.1770 K-weighting filter pair (coefficients for any sample rate,
    as used by libebur128). */
struct KWeighting
{
    Biquad shelf, hp;

    void prepare (double fs)
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0);
            const double Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            BiquadCoeffs c;
            c.b0 = (Vh + Vb * K / Q + K * K) / a0;
            c.b1 = 2.0 * (K * K - Vh) / a0;
            c.b2 = (Vh - Vb * K / Q + K * K) / a0;
            c.a1 = 2.0 * (K * K - 1.0) / a0;
            c.a2 = (1.0 - K / Q + K * K) / a0;
            shelf.set (c);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            BiquadCoeffs c;
            c.b0 = 1.0; c.b1 = -2.0; c.b2 = 1.0;
            c.a1 = 2.0 * (K * K - 1.0) / a0;
            c.a2 = (1.0 - K / Q + K * K) / a0;
            hp.set (c);
        }
        shelf.reset();
        hp.reset();
    }

    inline float process (float x) { return hp.process (shelf.process (x)); }
};

/** Running momentary (400 ms) and short-term (3 s) loudness in LUFS. */
class LoudnessMeter
{
public:
    void prepare (double fs, int numChannels)
    {
        k.assign ((size_t) numChannels, KWeighting());
        for (auto& f : k)
            f.prepare (fs);
        mCoeff = timeCoeff (400.0 / 2.0, fs);
        sCoeff = timeCoeff (3000.0 / 2.0, fs);
        mState = sState = 0.0;
    }

    void process (const float* const* ch, int numCh, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            double sum = 0.0;
            for (int c = 0; c < numCh && c < (int) k.size(); ++c)
            {
                const float y = k[(size_t) c].process (ch[c][i]);
                sum += (double) y * y;
            }
            mState = mCoeff * mState + (1.0 - mCoeff) * sum;
            sState = sCoeff * sState + (1.0 - sCoeff) * sum;
        }
    }

    float momentaryLufs() const { return toLufs (mState); }
    float shortTermLufs() const { return toLufs (sState); }

private:
    static float toLufs (double ms) { return (float) (-0.691 + 10.0 * std::log10 (std::max (ms, 1.0e-12))); }
    std::vector<KWeighting> k;
    double mCoeff = 0.0, sCoeff = 0.0, mState = 0.0, sState = 0.0;
};

class MixFit
{
public:
    bool enabled = true;
    float offsetDb = 0.0f;   // desired vocal loudness relative to instrumental
    float rideRangeDb = 6.0f;
    float unmaskDb = 4.0f;   // maximum dynamic presence lift

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        vocalK.prepare (fs);
        beatK.prepare (fs);
        vocalBand.set (makeBiquad (FilterType::BandPass, fs, 3000.0, 0.8));
        beatBand.set (makeBiquad (FilterType::BandPass, fs, 3000.0, 0.8));
        presence.assign ((size_t) numChannels, Biquad());
        vocalPow = beatPow = vocalBandPow = beatBandPow = 0.0;
        rideDb = boostDb = 0.0;
        blockCounter = 0;
    }

    /** sidechain may be nullptr (no instrumental connected). */
    void process (float* const* ch, int numCh, int n, const float* const* sc, int numScCh)
    {
        if (! enabled)
        {
            rideDb = boostDb = 0.0;
            return;
        }
        const double lvlCoeff = timeCoeff (400.0, fs);
        const double rideUp = timeCoeff (900.0, fs), rideDown = timeCoeff (350.0, fs);
        const double gateLin = 1.0e-6; // ~ -60 dBFS mean-square gate

        for (int i0 = 0; i0 < n; i0 += kBlock)
        {
            const int len = std::min (kBlock, n - i0);
            for (int i = i0; i < i0 + len; ++i)
            {
                float v = 0.0f, b = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    v += ch[c][i];
                v /= (float) std::max (1, numCh);
                if (sc != nullptr && numScCh > 0)
                {
                    for (int c = 0; c < numScCh; ++c)
                        b += sc[c][i];
                    b /= (float) numScCh;
                }
                const double vk = vocalK.process (v), bk = beatK.process (b);
                const double vb = vocalBand.process (v), bb = beatBand.process (b);
                vocalPow = lvlCoeff * vocalPow + (1.0 - lvlCoeff) * vk * vk;
                beatPow = lvlCoeff * beatPow + (1.0 - lvlCoeff) * bk * bk;
                vocalBandPow = lvlCoeff * vocalBandPow + (1.0 - lvlCoeff) * vb * vb;
                beatBandPow = lvlCoeff * beatBandPow + (1.0 - lvlCoeff) * bb * bb;
            }

            // Block-rate control.
            const bool active = vocalPow > gateLin && beatPow > gateLin;
            double targetRide = rideDb;  // hold while vocal is silent
            double targetBoost = 0.0;
            if (active)
            {
                const double vDb = 10.0 * std::log10 (vocalPow), bDb = 10.0 * std::log10 (beatPow);
                // Levels are measured before the ride gain is applied.
                targetRide = std::clamp ((bDb + offsetDb) - vDb, -(double) rideRangeDb, (double) rideRangeDb);

                // Masking: beat's presence-band level vs the (ridden) vocal's.
                // The vocal should sit ~6 dB above the beat in this band.
                const double vBandDb = 10.0 * std::log10 (std::max (vocalBandPow, 1e-12)) + rideDb;
                const double bBandDb = 10.0 * std::log10 (std::max (beatBandPow, 1e-12));
                const double masking = bBandDb - vBandDb + 6.0;
                targetBoost = std::clamp (masking * 0.5, 0.0, (double) unmaskDb);
            }
            else if (beatPow <= gateLin)
                targetRide = 0.0;

            const double blockTime = (double) len;
            const double ku = std::pow (targetRide > rideDb ? rideUp : rideDown, blockTime);
            rideDb = ku * rideDb + (1.0 - ku) * targetRide;
            const double kb = std::pow (rideDown, blockTime);
            boostDb = kb * boostDb + (1.0 - kb) * targetBoost;

            if (++blockCounter % 4 == 0 || std::abs (boostDb - appliedBoost) > 0.25)
            {
                appliedBoost = boostDb;
                const auto c = makeBiquad (FilterType::Peak, fs, 3200.0, 0.8, appliedBoost);
                for (auto& p : presence)
                    p.set (c);
            }

            const float g = (float) dbToGain (rideDb);
            for (int c = 0; c < numCh; ++c)
            {
                auto& p = presence[(size_t) std::min (c, (int) presence.size() - 1)];
                for (int i = i0; i < i0 + len; ++i)
                    ch[c][i] = p.process (ch[c][i]) * g;
            }
        }
    }

    float getRideDb() const { return (float) rideDb; }
    float getUnmaskDb() const { return (float) boostDb; }

private:
    static constexpr int kBlock = 32;
    double fs = 48000.0;
    KWeighting vocalK, beatK;
    Biquad vocalBand, beatBand;
    std::vector<Biquad> presence;
    double vocalPow = 0, beatPow = 0, vocalBandPow = 0, beatBandPow = 0;
    double rideDb = 0, boostDb = 0, appliedBoost = 0;
    long blockCounter = 0;
};

} // namespace se
