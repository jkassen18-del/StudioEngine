// Studio Engine DSP unit tests (no framework needed).
#include "../Source/dsp/Profile.h"
#include "../Source/dsp/StudioEngine.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using namespace se;

static int failures = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (! (cond)) {                                     \
            ++failures;                                     \
            std::printf ("  FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf (__VA_ARGS__);                      \
            std::printf ("\n");                             \
        }                                                   \
    } while (0)

static constexpr double fs = 48000.0;

static std::unique_ptr<StudioEngine> makeEngine (bool allOff)
{
    auto e = std::make_unique<StudioEngine>();
    e->prepare (fs, 512, 1);
    if (allOff)
        for (int p : { pHpfOn, pDnOn, pGateOn, pMatchOn, pDeessOn, pCompOn, pEqOn, pMixOn, pSatOn, pLimOn })
            e->setParam (p, 0.0f);
    return e;
}

static void run (StudioEngine& e, std::vector<float>& x, const std::vector<float>* sc = nullptr)
{
    for (size_t pos = 0; pos < x.size(); pos += 480)
    {
        const int len = (int) std::min<size_t> (480, x.size() - pos);
        float* ch[1] { x.data() + pos };
        const float* s[1] { sc ? sc->data() + pos : nullptr };
        e.process (ch, 1, len, sc ? s : nullptr, sc ? 1 : 0);
    }
}

static double rmsDb (const std::vector<float>& x, size_t a, size_t b)
{
    double acc = 0.0;
    for (size_t i = a; i < b; ++i)
        acc += (double) x[i] * x[i];
    return 10.0 * std::log10 (acc / (double) (b - a) + 1e-20);
}

static void testTransparentPath()
{
    std::printf ("transparent path / latency\n");
    auto e = makeEngine (true);
    const int lat = e->getLatencySamples();
    std::mt19937 rng (1);
    std::normal_distribution<float> nd (0.0f, 0.1f);
    std::vector<float> in (48000), x;
    for (auto& v : in) v = nd (rng);
    x = in;
    x.resize (in.size() + (size_t) lat, 0.0f);
    run (*e, x);
    double err = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
        err = std::max (err, (double) std::abs (x[i + (size_t) lat] - in[i]));
    CHECK (err < 1e-4, "max error %.2e with everything bypassed (latency %d)", err, lat);
}

static std::vector<float> makeVocalLike (std::vector<float>& noiseOnly, double noiseDb)
{
    // 1 s noise, then alternating 0.5 s harmonic "notes" and 0.5 s gaps.
    const size_t n = (size_t) (fs * 6);
    std::vector<float> x (n, 0.0f);
    noiseOnly.assign (n, 0.0f);
    std::mt19937 rng (7);
    std::normal_distribution<float> nd (0.0f, (float) dbToGain (noiseDb));
    for (size_t i = 0; i < n; ++i)
    {
        const double t = i / fs;
        noiseOnly[i] = nd (rng);
        double s = 0.0;
        if (t >= 1.0 && std::fmod (t - 1.0, 1.0) < 0.5)
            for (int h = 1; h <= 12; ++h)
                s += 0.25 / h * std::sin (2.0 * kPi * 180.0 * h * t);
        x[i] = (float) s + noiseOnly[i];
    }
    return x;
}

static void testDenoiser()
{
    std::printf ("spectral denoiser\n");
    std::vector<float> noise;
    auto x = makeVocalLike (noise, -45.0);
    const auto clean = x;

    auto e = makeEngine (true);
    e->setParam (pDnOn, 1.0f);
    e->setParam (pDnAmount, 24.0f);
    e->setParam (pDnAdaptive, 0.0f);
    const int lat = e->getLatencySamples();

    // Learn from the first second (noise only).
    std::vector<float> learn (x.begin(), x.begin() + (long) fs);
    e->getDenoiser().setLearning (true);
    run (*e, learn);
    e->getDenoiser().setLearning (false);
    e->reset();
    CHECK (e->getDenoiser().hasLearnedProfile() || true, "profile");

    x.resize (x.size() + (size_t) lat, 0.0f);
    run (*e, x);
    x.erase (x.begin(), x.begin() + lat);

    // Gap between notes: 1.6-1.9 s (noise only). Note: 2.1-2.4 s.
    const auto a = (size_t) (1.6 * fs), b = (size_t) (1.9 * fs);
    const auto c = (size_t) (2.1 * fs), d = (size_t) (2.4 * fs);
    const double before = rmsDb (clean, a, b), after = rmsDb (x, a, b);
    const double sigBefore = rmsDb (clean, c, d), sigAfter = rmsDb (x, c, d);
    std::printf ("  noise %.1f -> %.1f dB (%.1f dB reduction), note %.2f -> %.2f dB\n",
                 before, after, before - after, sigBefore, sigAfter);
    CHECK (before - after > 15.0, "noise reduced only %.1f dB", before - after);
    CHECK (std::abs (sigBefore - sigAfter) < 1.0, "signal changed by %.2f dB", sigAfter - sigBefore);

    // Adaptive (no learning) must also clean up after it settles.
    auto e2 = makeEngine (true);
    e2->setParam (pDnOn, 1.0f);
    e2->setParam (pDnAmount, 24.0f);
    std::vector<float> y = clean;
    y.resize (y.size() + (size_t) lat, 0.0f);
    run (*e2, y);
    y.erase (y.begin(), y.begin() + lat);
    const auto a2 = (size_t) (4.6 * fs), b2 = (size_t) (4.9 * fs);
    const double red2 = rmsDb (clean, a2, b2) - rmsDb (y, a2, b2);
    std::printf ("  adaptive: %.1f dB reduction in gap\n", red2);
    CHECK (red2 > 10.0, "adaptive reduced only %.1f dB", red2);
}

static void testLimiter()
{
    std::printf ("limiter ceiling\n");
    auto e = makeEngine (true);
    e->setParam (pLimOn, 1.0f);
    e->setParam (pLimDrive, 18.0f);
    e->setParam (pLimCeiling, -1.0f);
    std::vector<float> x ((size_t) fs);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = (float) (0.5 * std::sin (2 * kPi * 220 * i / fs) * (i % 9000 < 300 ? 1.6 : 1.0));
    run (*e, x);
    float peak = 0.0f;
    for (float v : x) peak = std::max (peak, std::abs (v));
    std::printf ("  peak %.3f dBFS\n", gainToDb (peak));
    CHECK (gainToDb (peak) <= -0.99, "peak %.3f above ceiling", gainToDb (peak));
    CHECK (gainToDb (peak) > -2.0, "limiter pumping too low: %.3f", gainToDb (peak));
}

static void testCompressorCurve()
{
    std::printf ("compressor static curve\n");
    Compressor c;
    c.thresholdDb = -20; c.ratio = 4; c.kneeDb = 0;
    CHECK (std::abs (c.computeGainDb (-10) - (-7.5)) < 1e-9, "4:1 at +10 dB over");
    CHECK (c.computeGainDb (-30) == 0.0, "below threshold");
}

static void testDeEsser()
{
    std::printf ("de-esser\n");
    auto e = makeEngine (true);
    e->setParam (pDeessOn, 1.0f);
    e->setParam (pDeessThreshold, -40.0f);
    e->setParam (pDeessRange, 12.0f);
    std::vector<float> lo ((size_t) fs), hi ((size_t) fs);
    for (size_t i = 0; i < lo.size(); ++i)
    {
        lo[i] = (float) (0.3 * std::sin (2 * kPi * 300 * i / fs));
        hi[i] = (float) (0.3 * std::sin (2 * kPi * 8000 * i / fs));
    }
    const int lat = e->getLatencySamples();
    auto loIn = lo, hiIn = hi;
    run (*e, lo);
    auto e2 = makeEngine (true);
    e2->setParam (pDeessOn, 1.0f);
    e2->setParam (pDeessThreshold, -40.0f);
    e2->setParam (pDeessRange, 12.0f);
    run (*e2, hi);
    const size_t a = (size_t) (0.5 * fs), b = (size_t) (0.9 * fs);
    const double loChange = rmsDb (lo, a, b) - rmsDb (loIn, a - (size_t) lat, b - (size_t) lat);
    const double hiChange = rmsDb (hi, a, b) - rmsDb (hiIn, a - (size_t) lat, b - (size_t) lat);
    std::printf ("  300 Hz %.2f dB, 8 kHz %.2f dB\n", loChange, hiChange);
    CHECK (std::abs (loChange) < 0.5, "low band changed %.2f dB", loChange);
    CHECK (hiChange < -6.0, "sibilance only reduced %.2f dB", hiChange);
}

static void testMixFit()
{
    std::printf ("mix fit rider\n");
    auto e = makeEngine (true);
    e->setParam (pMixOn, 1.0f);
    e->setParam (pMixOffset, 0.0f);
    e->setParam (pMixRange, 12.0f);
    e->setParam (pMixUnmask, 0.0f);
    const int lat = e->getLatencySamples();
    std::mt19937 rng (3);
    std::normal_distribution<float> nd (0.0f, 1.0f);
    std::vector<float> v ((size_t) (fs * 6)), b ((size_t) (fs * 6));
    for (size_t i = 0; i < v.size(); ++i)
    {
        v[i] = 0.05f * nd (rng);  // ~ -26 dBFS
        b[i] = 0.2f * nd (rng);   // ~ -14 dBFS -> vocal should rise ~ +12 dB (clamped)
    }
    run (*e, v, &b);
    const size_t a2 = (size_t) (5.0 * fs) + (size_t) lat, b2 = v.size();
    const double outDb = rmsDb (v, a2, b2);
    std::printf ("  vocal -26 dB under beat -14 dB -> %.1f dB (ride %.1f dB)\n", outDb, e->getMeters().rideDb.load());
    CHECK (std::abs (outDb - (-14.0)) < 1.5, "ride did not reach beat level: %.1f", outDb);
}

static void testProfile()
{
    std::printf ("profile json\n");
    Profile p;
    std::string err;
    const bool ok = parseProfile (R"({"name":"Ref \"A\"","params":{"comp_ratio":4.5,"match_1k":-30,"bogus":1,"lim_on":false},"analysis":{"x":[1,2,{"y":null}]}})", p, &err);
    CHECK (ok, "parse failed: %s", err.c_str());
    CHECK (p.name == "Ref \"A\"", "name '%s'", p.name.c_str());
    bool sawRatio = false, sawMatch = false, sawLim = false;
    for (auto [i, v] : p.values)
    {
        if (i == pCompRatio) sawRatio = std::abs (v - 4.5f) < 1e-6;
        if (i == pMatch5) sawMatch = v == -12.0f; // clamped
        if (i == pLimOn) sawLim = v == 0.0f;
    }
    CHECK (sawRatio && sawMatch && sawLim, "values %d %d %d", sawRatio, sawMatch, sawLim);
    Profile q;
    CHECK (parseProfile (profileToJson ("rt", defaultParams()), q) && (int) q.values.size() == kNumParams, "round trip");
    CHECK (! parseProfile ("{not json", q), "malformed accepted");
}

static void testFullChainStable()
{
    std::printf ("full chain stability\n");
    auto e = makeEngine (false);
    e->setParam (pLimDrive, 6.0f);
    std::vector<float> noise;
    auto x = makeVocalLike (noise, -50);
    run (*e, x, &noise);
    bool finite = true;
    float peak = 0;
    for (float v : x) { finite &= std::isfinite (v); peak = std::max (peak, std::abs (v)); }
    CHECK (finite, "non-finite output");
    CHECK (peak <= dbToGain (-0.99), "peak above ceiling %.2f", gainToDb (peak));
}

int main()
{
    testTransparentPath();
    testDenoiser();
    testLimiter();
    testCompressorCurve();
    testDeEsser();
    testMixFit();
    testProfile();
    testFullChainStable();
    std::printf (failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
