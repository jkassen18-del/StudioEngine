#pragma once
// Studio Engine - STFT spectral noise reduction.
//
//  * Noise estimate: either a learned profile (press "Learn" over a section of
//    pure background noise) and/or an adaptive minimum-statistics tracker that
//    follows slowly changing noise (fans, hum, room tone, hiss) automatically.
//  * Gain rule: decision-directed a-priori SNR (Ephraim & Malah) Wiener gain
//    with a user-set reduction floor. Gains are smoothed across frequency to
//    avoid "musical noise" artefacts.
//  * Latency: exactly one FFT frame (2048 samples @ 44.1/48 kHz, 4096 above).
#include "FFT.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>
#include <vector>

namespace se
{
class SpectralDenoiser
{
public:
    struct Settings
    {
        bool enabled = true;
        float reductionDb = 18.0f;  // maximum attenuation of noise (0..40)
        float sensitivityDb = 0.0f; // + = more aggressive (over-subtraction)
        bool adaptive = true;       // track noise automatically
    };

    /** Allocates everything. A learned noise profile survives re-preparation
        (hosts call prepare often) as long as the FFT size is unchanged; a
        learning pass still in progress is committed first. */
    void prepare (double sampleRate, int numChannels)
    {
        if (learning && learnCount > 0)
            commitLearning();
        learning = false;
        const int oldBins = bins;
        const bool keepProfile = learnedValid;
        fs = sampleRate;
        order = sampleRate > 50000.0 ? 12 : 11;
        fft.setOrder (order);
        N = 1 << order;
        hop = N / 4;
        bins = N / 2 + 1;

        window.resize ((size_t) N);
        for (int i = 0; i < N; ++i)
            window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / N));

        // Normalisation so analysis*synthesis windows overlap-add to unity.
        double olaSum = 0.0;
        for (int k = 0; k < N / hop; ++k)
            olaSum += (double) window[(size_t) (k * hop)] * window[(size_t) (k * hop)];
        olaNorm = (float) (1.0 / olaSum);

        // Minimum statistics: ~1.5 s search window split into U sub-windows.
        const double frameRate = fs / hop;
        subWinLen = std::max (4, (int) std::round (1.5 * frameRate / numSubWins));

        channels.assign ((size_t) std::max (1, numChannels), ChannelState());
        for (auto& ch : channels)
            ch.allocate (N, hop, bins, numSubWins);

        learnSum.assign ((size_t) bins, 0.0);
        learnCount = 0;
        if (! (keepProfile && oldBins == bins))
        {
            profileVersion.fetch_add (1, std::memory_order_acq_rel);
            learnedValid = false;
            learned.assign ((size_t) bins, 0.0f);
            profileVersion.fetch_add (1, std::memory_order_release);
        }
        scratch.resize ((size_t) N);
        gainTmp.resize ((size_t) bins);
        applyPendingProfile (true);
    }

    int getLatencySamples() const { return N; }
    int getFftSize() const { return N; }

    void setSettings (const Settings& s) { settings = s; }

    /** Toggle noise-profile learning (thread-safe; applied on audio thread). */
    void setLearning (bool shouldLearn) { learnRequest.store (shouldLearn); }
    bool isLearning() const { return learning; }
    bool hasLearnedProfile() const { return learnedValid.load(); }

    void clearLearnedProfile()
    {
        clearRequest.store (true);
    }

    /** Learned noise power spectrum (per bin), for saving with the session.
        Safe to call from any thread (seqlock read; the audio thread never
        waits). */
    std::vector<float> getLearnedProfile() const
    {
        for (;;)
        {
            const int v1 = profileVersion.load (std::memory_order_acquire);
            if (v1 & 1)
                continue;
            std::vector<float> copy = learnedValid.load() ? learned : std::vector<float>();
            std::atomic_thread_fence (std::memory_order_acquire);
            if (profileVersion.load (std::memory_order_relaxed) == v1)
                return copy;
        }
    }

    /** Restore a learned profile from any thread; it is applied by the audio
        thread (or the next prepare). Ignored if the FFT size differs. */
    void setLearnedProfile (const std::vector<float>& p)
    {
        std::lock_guard<std::mutex> lock (pendingMutex);
        pendingProfile = p;
        pendingFlag.store (true);
    }

    void reset()
    {
        for (auto& ch : channels)
            ch.clear();
    }

    /** Process one channel buffer in place. */
    void process (int channel, float* data, int numSamples)
    {
        handleRequests (channel);
        auto& ch = channels[(size_t) std::min (channel, (int) channels.size() - 1)];
        for (int i = 0; i < numSamples; ++i)
        {
            ch.inFifo[(size_t) ch.fifoPos] = data[i];
            data[i] = ch.outFifo[(size_t) ch.fifoPos];
            if (++ch.fifoPos == hop)
            {
                ch.fifoPos = 0;
                processFrame (ch, channel == 0);
            }
        }
    }

    /** Current mean gain reduction (dB, positive) for metering. */
    float getReductionDb() const { return meterReductionDb.load(); }

private:
    struct ChannelState
    {
        std::vector<float> inFifo, outFifo, frame, accum;
        std::vector<float> smoothPow, prevGain, prevPost, noiseAdaptive;
        std::vector<float> subMin, curMin; // subMin: bins * U
        int fifoPos = 0, subCount = 0, subIndex = 0;
        bool primed = false;

        void allocate (int n, int h, int b, int u)
        {
            inFifo.assign ((size_t) h, 0.0f);
            outFifo.assign ((size_t) h, 0.0f);
            frame.assign ((size_t) n, 0.0f);
            accum.assign ((size_t) n, 0.0f);
            smoothPow.assign ((size_t) b, 0.0f);
            prevGain.assign ((size_t) b, 1.0f);
            prevPost.assign ((size_t) b, 1.0f);
            noiseAdaptive.assign ((size_t) b, 0.0f);
            subMin.assign ((size_t) (b * u), 0.0f);
            curMin.assign ((size_t) b, 0.0f);
            fifoPos = subCount = subIndex = 0;
            primed = false;
        }

        void clear()
        {
            std::fill (inFifo.begin(), inFifo.end(), 0.0f);
            std::fill (outFifo.begin(), outFifo.end(), 0.0f);
            std::fill (frame.begin(), frame.end(), 0.0f);
            std::fill (accum.begin(), accum.end(), 0.0f);
            fifoPos = 0;
        }
    };

    void handleRequests (int channel)
    {
        if (channel != 0)
            return;
        applyPendingProfile (false);
        if (clearRequest.exchange (false))
        {
            learnedValid = false;
            learnCount = 0;
            std::fill (learnSum.begin(), learnSum.end(), 0.0);
        }
        const bool want = learnRequest.load();
        if (want && ! learning)
        {
            learning = true;
            learnCount = 0;
            std::fill (learnSum.begin(), learnSum.end(), 0.0);
        }
        else if (! want && learning)
        {
            learning = false;
            commitLearning();
        }
    }

    void commitLearning()
    {
        if (learnCount > 0)
        {
            profileVersion.fetch_add (1, std::memory_order_acq_rel);
            for (int k = 0; k < bins; ++k)
                learned[(size_t) k] = (float) (learnSum[(size_t) k] / learnCount);
            learnedValid = true;
            profileVersion.fetch_add (1, std::memory_order_release);
        }
        learnCount = 0;
    }

    void applyPendingProfile (bool blocking)
    {
        if (! pendingFlag.load())
            return;
        std::unique_lock<std::mutex> lock (pendingMutex, std::defer_lock);
        if (blocking)
            lock.lock();
        else if (! lock.try_lock())
            return; // retry next block, never wait on the audio thread
        if ((int) pendingProfile.size() == bins)
        {
            profileVersion.fetch_add (1, std::memory_order_acq_rel);
            std::copy (pendingProfile.begin(), pendingProfile.end(), learned.begin());
            learnedValid = true;
            profileVersion.fetch_add (1, std::memory_order_release);
        }
        pendingFlag.store (false);
    }

    void processFrame (ChannelState& ch, bool isMeterChannel)
    {
        // Slide the analysis frame and append the new hop.
        std::move (ch.frame.begin() + hop, ch.frame.end(), ch.frame.begin());
        std::copy (ch.inFifo.begin(), ch.inFifo.end(), ch.frame.end() - hop);

        for (int i = 0; i < N; ++i)
            scratch[(size_t) i] = { ch.frame[(size_t) i] * window[(size_t) i], 0.0f };
        fft.perform (scratch.data(), false);

        const float minGain = settings.enabled ? std::pow (10.0f, -settings.reductionDb / 20.0f) : 1.0f;
        const float overSub = std::pow (10.0f, settings.sensitivityDb / 10.0f);
        const bool useAdaptive = settings.adaptive || ! learnedValid;

        // --- Noise estimation -------------------------------------------------
        const float alphaS = 0.85f;
        ++ch.subCount;
        const bool subWinDone = ch.subCount >= subWinLen;
        for (int k = 0; k < bins; ++k)
        {
            const float p = std::norm (scratch[(size_t) k]);
            if (learning)
                learnSum[(size_t) k] += p;

            float& s = ch.smoothPow[(size_t) k];
            s = ch.primed ? alphaS * s + (1.0f - alphaS) * p : p;

            float& cm = ch.curMin[(size_t) k];
            cm = (ch.subCount == 1 || ! ch.primed) ? s : std::min (cm, s);

            if (! ch.primed)
                for (int u = 0; u < numSubWins; ++u)
                    ch.subMin[(size_t) (k * numSubWins + u)] = s;

            float m = cm;
            for (int u = 0; u < numSubWins; ++u)
                m = std::min (m, ch.subMin[(size_t) (k * numSubWins + u)]);
            ch.noiseAdaptive[(size_t) k] = m * minStatBias;

            if (subWinDone)
                ch.subMin[(size_t) (k * numSubWins + ch.subIndex)] = cm;
        }
        if (learning)
            ++learnCount;
        if (subWinDone)
        {
            ch.subCount = 0;
            ch.subIndex = (ch.subIndex + 1) % numSubWins;
        }
        ch.primed = true;

        // --- Gain computation (decision-directed Wiener) ----------------------
        const float dd = 0.98f;
        for (int k = 0; k < bins; ++k)
        {
            float noise;
            if (learnedValid && useAdaptive)
                noise = std::sqrt (learned[(size_t) k] * ch.noiseAdaptive[(size_t) k]);
            else if (learnedValid)
                noise = learned[(size_t) k];
            else
                noise = ch.noiseAdaptive[(size_t) k];

            noise = std::max (noise * overSub, 1.0e-20f);
            const float p = std::norm (scratch[(size_t) k]);
            const float post = p / noise;
            const float prio = dd * ch.prevGain[(size_t) k] * ch.prevGain[(size_t) k] * ch.prevPost[(size_t) k]
                               + (1.0f - dd) * std::max (post - 1.0f, 0.0f);
            const float g = prio / (1.0f + prio);
            ch.prevPost[(size_t) k] = std::min (post, 1.0e6f);
            ch.prevGain[(size_t) k] = g;
            gainTmp[(size_t) k] = g;
        }

        // Frequency smoothing (3-tap) then apply floor.
        double meterAcc = 0.0;
        float prev = gainTmp[0];
        for (int k = 0; k < bins; ++k)
        {
            const float cur = gainTmp[(size_t) k];
            const float next = k + 1 < bins ? gainTmp[(size_t) k + 1] : cur;
            float g = 0.25f * prev + 0.5f * cur + 0.25f * next;
            prev = cur;
            g = minGain + (1.0f - minGain) * g;
            meterAcc += g;
            scratch[(size_t) k] *= g;
            if (k > 0 && k < N / 2)
                scratch[(size_t) (N - k)] = std::conj (scratch[(size_t) k]);
        }
        if (isMeterChannel)
            meterReductionDb.store ((float) -20.0 * std::log10 ((float) std::max (1.0e-6, meterAcc / bins)));

        fft.perform (scratch.data(), true);

        const float scale = olaNorm / (float) N;
        for (int i = 0; i < N; ++i)
            ch.accum[(size_t) i] += scratch[(size_t) i].real() * window[(size_t) i] * scale;

        std::copy (ch.accum.begin(), ch.accum.begin() + hop, ch.outFifo.begin());
        std::move (ch.accum.begin() + hop, ch.accum.end(), ch.accum.begin());
        std::fill (ch.accum.end() - hop, ch.accum.end(), 0.0f);
    }

    static constexpr int numSubWins = 8;
    static constexpr float minStatBias = 1.5f;

    Settings settings;
    double fs = 48000.0;
    int order = 11, N = 2048, hop = 512, bins = 1025, subWinLen = 16;
    float olaNorm = 1.0f;
    FFT fft;
    std::vector<float> window, gainTmp;
    std::vector<std::complex<float>> scratch;
    std::vector<ChannelState> channels;

    std::vector<double> learnSum;
    std::vector<float> learned;
    long learnCount = 0;
    std::atomic<bool> learning { false };
    std::atomic<bool> learnedValid { false };
    std::atomic<int> profileVersion { 0 };
    std::mutex pendingMutex;
    std::vector<float> pendingProfile;
    std::atomic<bool> pendingFlag { false };
    std::atomic<bool> learnRequest { false }, clearRequest { false };
    std::atomic<float> meterReductionDb { 0.0f };
};
} // namespace se
