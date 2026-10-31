#pragma once
// Studio Engine - the complete vocal clean-up + mastering chain.
//
//   Input gain -> HPF -> Spectral denoise -> Gate -> Match EQ -> De-esser
//   -> Compressor -> Tone EQ -> Mix Fit (side-chain: instrumental)
//   -> Saturation -> Limiter -> Output gain
//
// Framework independent: used by the JUCE plugin and the command-line renderer
// (which the reference-match tool drives), so both produce identical audio.
#include "Dynamics.h"
#include "EQ.h"
#include "MixFit.h"
#include "Params.h"
#include "SpectralDenoiser.h"
#include <array>
#include <atomic>
#include <vector>

namespace se
{
struct Meters
{
    std::atomic<float> inputLufs { -100.f }, outputLufs { -100.f };
    std::atomic<float> denoiseDb { 0.f }, gateDb { 0.f }, deessDb { 0.f }, compDb { 0.f }, limiterDb { 0.f };
    std::atomic<float> rideDb { 0.f }, unmaskDb { 0.f };
};

class StudioEngine
{
public:
    StudioEngine() { values = defaultParams(); }

    void prepare (double sampleRate, int maxBlockSize, int numChannels)
    {
        fs = sampleRate;
        maxBlock = std::max (32, maxBlockSize);
        nCh = std::max (1, numChannels);

        hpf.prepare (fs, nCh);
        denoiser.prepare (fs, nCh);
        gate.prepare (fs);
        match.prepare (fs, nCh);
        deesser.prepare (fs, nCh);
        comp.prepare (fs);
        tone.prepare (fs, nCh);
        mixFit.prepare (fs, nCh);
        sat.prepare (fs, nCh);
        limiter.prepare (fs, nCh);
        inMeter.prepare (fs, nCh);
        outMeter.prepare (fs, nCh);

        // Side-chain is delayed by the denoiser latency to stay aligned.
        scDelayLen = denoiser.getLatencySamples();
        scDelay.assign (2, std::vector<float> ((size_t) scDelayLen, 0.0f));
        scBuf.assign (2, std::vector<float> ((size_t) maxBlock, 0.0f));
        scDelayPos = 0;

        inGainState = dbToGain (values[pInputGain]);
        outGainState = dbToGain (values[pOutputGain]);
        lastApplied.fill (-12345.f);
        applyParams();
    }

    void reset()
    {
        const auto learned = denoiser.getLearnedProfile();
        prepare (fs, maxBlock, nCh);
        if (! learned.empty())
            denoiser.setLearnedProfile (learned);
    }

    int getLatencySamples() const { return denoiser.getLatencySamples() + limiter.getLatencySamples(); }

    void setParam (int index, float v) { values[(size_t) index] = v; }
    float getParam (int index) const { return values[(size_t) index]; }
    const std::array<float, kNumParams>& getParams() const { return values; }

    SpectralDenoiser& getDenoiser() { return denoiser; }
    Meters& getMeters() { return meters; }

    /** Process `n` samples in place. sc/numScCh = instrumental side-chain
        (nullptr / 0 when not connected). */
    void process (float* const* ch, int numCh, int n, const float* const* sc = nullptr, int numScCh = 0)
    {
        numCh = std::min (numCh, nCh);
        int done = 0;
        while (done < n)
        {
            const int len = std::min (maxBlock, n - done);
            float* chunk[8] {};
            const float* scChunk[8] {};
            for (int c = 0; c < numCh && c < 8; ++c)
                chunk[c] = ch[c] + done;
            for (int c = 0; c < numScCh && c < 8; ++c)
                scChunk[c] = sc[c] + done;
            processBlock (chunk, std::min (numCh, 8), len, sc ? scChunk : nullptr, std::min (numScCh, 8));
            done += len;
        }
    }

private:
    static bool on (float v) { return v >= 0.5f; }

    void applyParams()
    {
        const auto& v = values;
        auto changed = [this] (int a, int b) {
            bool c = false;
            for (int i = a; i <= b; ++i)
                if (lastApplied[(size_t) i] != values[(size_t) i])
                {
                    c = true;
                    lastApplied[(size_t) i] = values[(size_t) i];
                }
            return c;
        };

        hpf.enabled = on (v[pHpfOn]);
        if (changed (pHpfFreq, pHpfFreq)) { hpf.frequency = v[pHpfFreq]; hpf.markDirty(); }

        SpectralDenoiser::Settings ds;
        ds.enabled = on (v[pDnOn]);
        ds.reductionDb = v[pDnAmount];
        ds.sensitivityDb = v[pDnSensitivity];
        ds.adaptive = on (v[pDnAdaptive]);
        denoiser.setSettings (ds);

        gate.enabled = on (v[pGateOn]);
        gate.thresholdDb = v[pGateThreshold];
        gate.rangeDb = v[pGateRange];
        gate.attackMs = v[pGateAttack];
        gate.releaseMs = v[pGateRelease];

        match.enabled = on (v[pMatchOn]);
        if (changed (pMatchAmount, pMatch9))
        {
            match.amount = v[pMatchAmount] / 100.0f;
            for (int b = 0; b < kMatchBands; ++b)
                match.gainsDb[(size_t) b] = v[(size_t) (pMatch0 + b)];
            match.markDirty();
        }

        deesser.enabled = on (v[pDeessOn]);
        deesser.frequency = v[pDeessFreq];
        deesser.thresholdDb = v[pDeessThreshold];
        deesser.rangeDb = v[pDeessRange];

        comp.enabled = on (v[pCompOn]);
        comp.thresholdDb = v[pCompThreshold];
        comp.ratio = v[pCompRatio];
        comp.attackMs = v[pCompAttack];
        comp.releaseMs = v[pCompRelease];
        comp.kneeDb = v[pCompKnee];
        comp.makeupDb = v[pCompMakeup];
        comp.mix = v[pCompMix] / 100.0f;

        tone.enabled = on (v[pEqOn]);
        if (changed (pEqLowFreq, pEqAirGain))
        {
            tone.lowFreq = v[pEqLowFreq]; tone.lowGain = v[pEqLowGain];
            tone.mudFreq = v[pEqMudFreq]; tone.mudGain = v[pEqMudGain];
            tone.presenceFreq = v[pEqPresenceFreq]; tone.presenceGain = v[pEqPresenceGain];
            tone.airFreq = v[pEqAirFreq]; tone.airGain = v[pEqAirGain];
            tone.markDirty();
        }

        mixFit.enabled = on (v[pMixOn]);
        mixFit.offsetDb = v[pMixOffset];
        mixFit.rideRangeDb = v[pMixRange];
        mixFit.unmaskDb = v[pMixUnmask];

        sat.enabled = on (v[pSatOn]);
        sat.driveDb = v[pSatDrive];
        sat.mix = v[pSatMix] / 100.0f;

        limiter.enabled = on (v[pLimOn]);
        limiter.driveDb = v[pLimDrive];
        limiter.ceilingDb = v[pLimCeiling];
        limiter.releaseMs = v[pLimRelease];
    }

    static void rampGain (float* const* ch, int numCh, int n, double& state, double target)
    {
        const double step = (target - state) / std::max (1, n);
        for (int i = 0; i < n; ++i)
        {
            state += step;
            for (int c = 0; c < numCh; ++c)
                ch[c][i] *= (float) state;
        }
        state = target;
    }

    void processBlock (float* const* ch, int numCh, int n, const float* const* sc, int numScCh)
    {
        applyParams();

        rampGain (ch, numCh, n, inGainState, dbToGain (values[pInputGain]));
        inMeter.process (ch, numCh, n);

        hpf.process (ch, numCh, n);
        for (int c = 0; c < numCh; ++c)
            denoiser.process (c, ch[c], n);
        gate.process (ch, numCh, n);
        match.process (ch, numCh, n);
        deesser.process (ch, numCh, n);
        comp.process (ch, numCh, n);
        tone.process (ch, numCh, n);

        // Delay the side-chain to line up with the (denoiser-delayed) vocal.
        const float* scPtrs[2] { nullptr, nullptr };
        const int scCh = sc != nullptr ? std::min (numScCh, 2) : 0;
        for (int c = 0; c < scCh; ++c)
        {
            auto& d = scDelay[(size_t) c];
            auto& out = scBuf[(size_t) c];
            int pos = scDelayPos;
            for (int i = 0; i < n; ++i)
            {
                out[(size_t) i] = d[(size_t) pos];
                d[(size_t) pos] = sc[c][i];
                if (++pos == scDelayLen)
                    pos = 0;
            }
            scPtrs[c] = out.data();
        }
        if (scCh > 0)
            scDelayPos = (scDelayPos + n) % scDelayLen;
        mixFit.process (ch, numCh, n, scCh > 0 ? scPtrs : nullptr, scCh);

        sat.process (ch, numCh, n);
        limiter.process (ch, numCh, n);
        rampGain (ch, numCh, n, outGainState, dbToGain (values[pOutputGain]));
        outMeter.process (ch, numCh, n);

        meters.inputLufs.store (inMeter.momentaryLufs());
        meters.outputLufs.store (outMeter.momentaryLufs());
        meters.denoiseDb.store (on (values[pDnOn]) ? denoiser.getReductionDb() : 0.f);
        meters.gateDb.store (gate.lastGrDb);
        meters.deessDb.store (deesser.lastGrDb);
        meters.compDb.store (comp.lastGrDb);
        meters.limiterDb.store (limiter.lastGrDb);
        meters.rideDb.store (mixFit.getRideDb());
        meters.unmaskDb.store (mixFit.getUnmaskDb());
    }

    double fs = 48000.0;
    int maxBlock = 512, nCh = 2;
    std::array<float, kNumParams> values {}, lastApplied {};

    HighPass hpf;
    SpectralDenoiser denoiser;
    Gate gate;
    MatchEQ match;
    DeEsser deesser;
    Compressor comp;
    ToneEQ tone;
    MixFit mixFit;
    Saturator sat;
    Limiter limiter;
    LoudnessMeter inMeter, outMeter;
    Meters meters;

    std::vector<std::vector<float>> scDelay, scBuf;
    int scDelayLen = 1, scDelayPos = 0;
    double inGainState = 1.0, outGainState = 1.0;
};

} // namespace se
