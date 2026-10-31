#pragma once
// Studio Engine - dynamics processors: gate/expander, de-esser, compressor,
// saturator and look-ahead brickwall limiter. All are stereo-linked.
#include "Biquad.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace se
{
//==============================================================================
/** Downward expander / gate with hold and hysteresis. Removes leftover room
    noise, breaths and bleed between phrases. */
class Gate
{
public:
    float thresholdDb = -50.0f, rangeDb = 20.0f, attackMs = 2.0f, releaseMs = 120.0f, holdMs = 40.0f;
    bool enabled = true;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        env = 0.0;
        gain = 1.0;
        holdCounter = 0;
    }

    /** Process interleaved-by-pointer channels in place. */
    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
        {
            lastGrDb = 0.0f;
            return;
        }
        const double envRel = timeCoeff (20.0, fs);
        const double att = timeCoeff (attackMs, fs);
        const double rel = timeCoeff (releaseMs, fs);
        const double open = dbToGain (thresholdDb);
        const double close = dbToGain (thresholdDb - 4.0); // hysteresis
        const double floorGain = dbToGain (-rangeDb);
        const int holdSamples = (int) (holdMs * 0.001 * fs);

        for (int i = 0; i < n; ++i)
        {
            double peak = 0.0;
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, (double) std::abs (ch[c][i]));
            env = peak > env ? peak : envRel * env + (1.0 - envRel) * peak;

            if (env >= open)
            {
                isOpen = true;
                holdCounter = holdSamples;
            }
            else if (env < close)
            {
                if (holdCounter > 0)
                    --holdCounter;
                else
                    isOpen = false;
            }

            double target = 1.0;
            if (! isOpen)
            {
                // 1:4 expansion below threshold, limited by range.
                const double below = gainToDb (env) - thresholdDb;
                target = std::max (floorGain, dbToGain (std::min (0.0, below * 3.0)));
            }
            const double coeff = target > gain ? att : rel;
            gain = coeff * gain + (1.0 - coeff) * target;
            for (int c = 0; c < numCh; ++c)
                ch[c][i] *= (float) gain;
        }
        lastGrDb = (float) -gainToDb (gain);
    }

    float lastGrDb = 0.0f;

private:
    double fs = 48000.0, env = 0.0, gain = 1.0;
    int holdCounter = 0;
    bool isOpen = true;
};

//==============================================================================
/** Dynamic de-esser: a high shelf at `frequency` is pulled down only while
    sibilance exceeds the threshold. At 0 dB reduction the shelf is an exact
    bypass, so the rest of the voice is untouched. */
class DeEsser
{
public:
    float frequency = 6500.0f, thresholdDb = -28.0f, rangeDb = 8.0f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        shelf.assign ((size_t) numChannels, Biquad());
        detect.reset();
        env = 0.0;
        grDb = 0.0;
        appliedDb = 0.0;
        lastFreq = -1.0f;
    }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
        {
            lastGrDb = 0.0f;
            grDb = 0.0;
            return;
        }
        if (frequency != lastFreq)
        {
            lastFreq = frequency;
            detect.set (makeBiquad (FilterType::BandPass, fs, std::min (frequency * 1.25, fs * 0.45), 1.0));
            updateShelf (true);
        }
        const double att = timeCoeff (0.5, fs), rel = timeCoeff (60.0, fs);
        const double gAtt = timeCoeff (1.0, fs), gRel = timeCoeff (50.0, fs);

        for (int i0 = 0; i0 < n; i0 += kSub)
        {
            const int end = std::min (n, i0 + kSub);
            for (int i = i0; i < end; ++i)
            {
                float mono = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    mono += ch[c][i];
                mono /= (float) numCh;
                const double s = std::abs ((double) detect.process (mono));
                const double k = s > env ? att : rel;
                env = k * env + (1.0 - k) * s;

                // Band-pass of a sine reads ~ -3 dB of its level (mean abs).
                const double over = gainToDb (env * 1.41) - thresholdDb;
                const double targetDb = over > 0.0 ? -std::min ((double) rangeDb, over * 0.75) : 0.0;
                const double kg = targetDb < grDb ? gAtt : gRel;
                grDb = kg * grDb + (1.0 - kg) * targetDb;
            }
            updateShelf (false);
            for (int c = 0; c < numCh; ++c)
            {
                auto& bq = shelf[(size_t) c];
                for (int i = i0; i < end; ++i)
                    ch[c][i] = bq.process (ch[c][i]);
            }
        }
        lastGrDb = (float) -grDb;
    }

    float lastGrDb = 0.0f;

private:
    void updateShelf (bool force)
    {
        if (! force && std::abs (grDb - appliedDb) < 0.05)
            return;
        appliedDb = grDb;
        const auto c = makeBiquad (FilterType::HighShelf, fs, frequency, 0.7071, appliedDb);
        for (auto& b : shelf)
            b.set (c);
    }

    static constexpr int kSub = 16;
    double fs = 48000.0, env = 0.0, grDb = 0.0, appliedDb = 0.0;
    float lastFreq = -1.0f;
    std::vector<Biquad> shelf;
    Biquad detect;
};

//==============================================================================
/** Feed-forward soft-knee compressor (log-domain smoothing), with parallel mix. */
class Compressor
{
public:
    float thresholdDb = -20.0f, ratio = 3.0f, attackMs = 8.0f, releaseMs = 120.0f, kneeDb = 6.0f;
    float makeupDb = 0.0f, mix = 1.0f;
    bool enabled = true;

    void prepare (double sampleRate)
    {
        fs = sampleRate;
        grDb = 0.0;
        rmsState = 0.0;
    }

    double computeGainDb (double levelDb) const
    {
        const double over = levelDb - thresholdDb;
        const double slope = 1.0 - 1.0 / std::max (1.0f, ratio);
        if (2.0 * over < -kneeDb)
            return 0.0;
        if (kneeDb > 0.0f && 2.0 * std::abs (over) <= kneeDb)
        {
            const double x = over + kneeDb / 2.0;
            return -slope * x * x / (2.0 * kneeDb);
        }
        return -slope * over;
    }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled)
        {
            lastGrDb = 0.0f;
            return;
        }
        const double att = timeCoeff (attackMs, fs), rel = timeCoeff (releaseMs, fs);
        const double rmsCoeff = timeCoeff (5.0, fs);
        const double makeup = dbToGain (makeupDb);
        const double wet = std::clamp (mix, 0.0f, 1.0f);

        for (int i = 0; i < n; ++i)
        {
            double sq = 0.0;
            for (int c = 0; c < numCh; ++c)
                sq = std::max (sq, (double) ch[c][i] * ch[c][i]);
            rmsState = rmsCoeff * rmsState + (1.0 - rmsCoeff) * sq;
            // Blend peak and short RMS for a natural vocal response.
            const double det = std::max (std::sqrt (rmsState) * 1.41421356, std::sqrt (sq) * 0.7);
            const double target = computeGainDb (gainToDb (det));
            const double k = target < grDb ? att : rel;
            grDb = k * grDb + (1.0 - k) * target;
            const double g = dbToGain (grDb) * makeup;
            const double mixG = wet * g + (1.0 - wet) * makeup;
            for (int c = 0; c < numCh; ++c)
                ch[c][i] *= (float) mixG;
        }
        lastGrDb = (float) -grDb;
    }

    float lastGrDb = 0.0f;

private:
    double fs = 48000.0, grDb = 0.0, rmsState = 0.0;
};

//==============================================================================
/** Gentle analogue-style saturation for density and harmonics. */
class Saturator
{
public:
    float driveDb = 3.0f, mix = 0.3f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        dcX.assign ((size_t) numChannels, 0.0);
        dcY.assign ((size_t) numChannels, 0.0);
        dcR = 1.0 - (2.0 * kPi * 10.0 / sampleRate);
    }

    void process (float* const* ch, int numCh, int n)
    {
        if (! enabled || mix <= 0.0f)
            return;
        const double k = dbToGain (driveDb);
        const double bias = 0.12;
        const double offset = std::tanh (k * bias);
        const double wet = std::clamp (mix, 0.0f, 1.0f);
        for (int c = 0; c < numCh; ++c)
        {
            double& x1 = dcX[(size_t) c];
            double& y1 = dcY[(size_t) c];
            for (int i = 0; i < n; ++i)
            {
                const double x = ch[c][i];
                double s = (std::tanh (k * (x + bias)) - offset) / k;
                // DC blocker removes the offset the asymmetry introduces.
                const double y = s - x1 + dcR * y1;
                x1 = s;
                y1 = y;
                ch[c][i] = (float) (wet * y + (1.0 - wet) * x);
            }
        }
    }

private:
    std::vector<double> dcX, dcY;
    double dcR = 0.999;
};

//==============================================================================
/** Look-ahead brickwall limiter. Gain is the sliding minimum of the required
    gain, released exponentially, then box-filtered over the look-ahead so the
    gain ramps smoothly into each peak and never overshoots the ceiling. */
class Limiter
{
public:
    float driveDb = 0.0f, ceilingDb = -1.0f, releaseMs = 80.0f;
    bool enabled = true;

    void prepare (double sampleRate, int numChannels)
    {
        fs = sampleRate;
        L = std::max (8, (int) std::round (0.0015 * fs));
        delay.assign ((size_t) numChannels, std::vector<float> ((size_t) L, 0.0f));
        minQ.assign ((size_t) L + 2, { 0, 1.0 });
        box.assign ((size_t) L, 1.0);
        boxSum = (double) L;
        qHead = qTail = 0;
        writePos = 0;
        sampleIndex = 0;
        relState = 1.0;
        boxPos = 0;
    }

    int getLatencySamples() const { return L - 1; }

    void process (float* const* ch, int numCh, int n)
    {
        const double drive = enabled ? dbToGain (driveDb) : 1.0;
        const double ceil = dbToGain (ceilingDb);
        const double rel = timeCoeff (releaseMs, fs);
        float minGain = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            double peak = 0.0;
            for (int c = 0; c < numCh; ++c)
                peak = std::max (peak, (double) std::abs (ch[c][i]) * drive);
            const double req = (enabled && peak > ceil) ? ceil / peak : 1.0;

            // Sliding minimum over the last L samples (monotonic deque).
            while (qTail != qHead && back().second >= req)
                popBack();
            pushBack ({ sampleIndex, req });
            while (front().first <= sampleIndex - L)
                popFront();
            const double held = front().second;

            relState = held < relState ? held : rel * relState + (1.0 - rel) * held;
            relState = std::min (relState, 1.0);

            boxSum += relState - box[(size_t) boxPos];
            box[(size_t) boxPos] = relState;
            boxPos = (boxPos + 1) % L;
            double g = boxSum / L;
            if (++sampleIndex % 65536 == 0) // refresh running sum (drift)
            {
                boxSum = 0.0;
                for (double v : box)
                    boxSum += v;
            }

            // Output the sample from L-1 samples ago.
            const int readPos = (writePos + 1) % L;
            for (int c = 0; c < numCh; ++c)
            {
                auto& d = delay[(size_t) c];
                d[(size_t) writePos] = ch[c][i] * (float) drive;
                const double out = d[(size_t) readPos] * g;
                ch[c][i] = (float) std::clamp (out, -ceil, ceil); // safety
                if (! enabled)
                    ch[c][i] = d[(size_t) readPos];
            }
            writePos = readPos;
            minGain = std::min (minGain, (float) g);
        }
        lastGrDb = (float) -gainToDb (minGain);
    }

    float lastGrDb = 0.0f;

private:
    using Entry = std::pair<long long, double>;
    Entry& front() { return minQ[(size_t) qHead]; }
    Entry& back() { return minQ[(size_t) ((qTail + cap() - 1) % cap())]; }
    int cap() const { return (int) minQ.size(); }
    void pushBack (Entry e)
    {
        minQ[(size_t) qTail] = e;
        qTail = (qTail + 1) % cap();
    }
    void popBack() { qTail = (qTail + cap() - 1) % cap(); }
    void popFront() { qHead = (qHead + 1) % cap(); }

    double fs = 48000.0;
    int L = 72, writePos = 0, boxPos = 0, qHead = 0, qTail = 0;
    long long sampleIndex = 0;
    double relState = 1.0, boxSum = 0.0;
    std::vector<std::vector<float>> delay;
    std::vector<Entry> minQ;
    std::vector<double> box;
};

} // namespace se
