#pragma once
// Studio Engine - equalisers: clean-up HPF, reference "Match EQ" and tone EQ.
#include "Biquad.h"
#include <array>
#include <vector>

namespace se
{
/** A bank of biquads applied in series to every channel. */
class FilterBank
{
public:
    void prepare (double sampleRate, int numChannels, int numFilters)
    {
        fs = sampleRate;
        filters.assign ((size_t) numChannels, std::vector<Biquad> ((size_t) numFilters));
        active.assign ((size_t) numFilters, false);
    }

    void setFilter (int index, const BiquadCoeffs& c, bool isActive)
    {
        for (auto& chain : filters)
            chain[(size_t) index].set (c);
        active[(size_t) index] = isActive;
    }

    void process (float* const* ch, int numCh, int n)
    {
        for (int c = 0; c < numCh && c < (int) filters.size(); ++c)
            for (size_t f = 0; f < active.size(); ++f)
                if (active[f])
                {
                    auto& bq = filters[(size_t) c][f];
                    for (int i = 0; i < n; ++i)
                        ch[c][i] = bq.process (ch[c][i]);
                }
    }

    double sampleRate() const { return fs; }

private:
    double fs = 48000.0;
    std::vector<std::vector<Biquad>> filters;
    std::vector<bool> active;
};

/** Octave-band centre frequencies used by the Match EQ and the analyser. */
constexpr int kMatchBands = 10;
inline const std::array<double, kMatchBands>& matchBandFrequencies()
{
    static const std::array<double, kMatchBands> f { 31.25, 62.5, 125.0, 250.0, 500.0,
                                                      1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };
    return f;
}
constexpr double kMatchBandQ = 1.41;

/** Match EQ: 10 octave-spaced peaking filters whose gains come from the
    reference analyser (tools/reference_match). `amount` scales the curve. */
class MatchEQ
{
public:
    std::array<float, kMatchBands> gainsDb {};
    float amount = 1.0f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        bank.prepare (sampleRate, numChannels, kMatchBands);
        dirty = true;
    }

    void markDirty() { dirty = true; }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
            return;
        if (dirty)
        {
            dirty = false;
            const auto& freqs = matchBandFrequencies();
            for (int b = 0; b < kMatchBands; ++b)
            {
                const double g = gainsDb[(size_t) b] * amount;
                const bool usable = freqs[(size_t) b] < bank.sampleRate() * 0.45;
                bank.setFilter (b, makeBiquad (FilterType::Peak, bank.sampleRate(), freqs[(size_t) b], kMatchBandQ, g),
                                usable && std::abs (g) > 0.01);
            }
        }
        bank.process (ch, numCh, n);
    }

private:
    FilterBank bank;
    bool dirty = true;
};

/** Musical 4-band tone EQ: low shelf, mud cut, presence, air shelf. */
class ToneEQ
{
public:
    float lowFreq = 120.0f, lowGain = 0.0f;
    float mudFreq = 350.0f, mudGain = 0.0f;
    float presenceFreq = 3000.0f, presenceGain = 0.0f;
    float airFreq = 10000.0f, airGain = 0.0f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        bank.prepare (sampleRate, numChannels, 4);
        dirty = true;
    }

    void markDirty() { dirty = true; }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
            return;
        if (dirty)
        {
            dirty = false;
            const double fs = bank.sampleRate();
            bank.setFilter (0, makeBiquad (FilterType::LowShelf, fs, lowFreq, 0.7071, lowGain), std::abs (lowGain) > 0.01f);
            bank.setFilter (1, makeBiquad (FilterType::Peak, fs, mudFreq, 1.0, mudGain), std::abs (mudGain) > 0.01f);
            bank.setFilter (2, makeBiquad (FilterType::Peak, fs, presenceFreq, 0.9, presenceGain), std::abs (presenceGain) > 0.01f);
            bank.setFilter (3, makeBiquad (FilterType::HighShelf, fs, airFreq, 0.7071, airGain), std::abs (airGain) > 0.01f);
        }
        bank.process (ch, numCh, n);
    }

private:
    FilterBank bank;
    bool dirty = true;
};

/** 24 dB/oct Butterworth high-pass for rumble / plosive / handling noise. */
class HighPass
{
public:
    float frequency = 80.0f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        bank.prepare (sampleRate, numChannels, 2);
        dirty = true;
    }
    void markDirty() { dirty = true; }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
            return;
        if (dirty)
        {
            dirty = false;
            const double fs = bank.sampleRate();
            bank.setFilter (0, makeBiquad (FilterType::HighPass, fs, frequency, 0.5412), true);
            bank.setFilter (1, makeBiquad (FilterType::HighPass, fs, frequency, 1.3066), true);
        }
        bank.process (ch, numCh, n);
    }

private:
    FilterBank bank;
    bool dirty = true;
};

} // namespace se
