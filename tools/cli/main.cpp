// studio_engine_cli - offline renderer for the Studio Engine chain.
//
// Runs exactly the same DSP as the plugin, so it is used by the reference
// matcher (tools/reference_match/studio_match.py) to render, measure and
// refine settings, and is handy for batch-cleaning files.
//
//   studio_engine_cli in.wav out.wav [options]
//     --profile FILE.json     load a Studio Engine profile
//     --set id=value          override one parameter (repeatable)
//     --sidechain BEAT.wav    instrumental for Mix Fit
//     --learn-noise S:E       learn the noise profile from seconds S..E
//     --auto-noise            find the quietest 0.5 s and learn from it
//     --pcm24                 write 24-bit PCM instead of 32-bit float
//     --list-params           print all parameter ids and ranges
#include "../../Source/dsp/Profile.h"
#include "../../Source/dsp/StudioEngine.h"
#include "WavFile.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace se;

static int usage()
{
    std::fprintf (stderr,
                  "usage: studio_engine_cli in.wav out.wav [--profile p.json] [--set id=value]...\n"
                  "                         [--sidechain beat.wav] [--learn-noise S:E] [--auto-noise]\n"
                  "                         [--pcm24]\n"
                  "       studio_engine_cli --list-params\n");
    return 2;
}

/** Locate the quietest window (by RMS) with at least some signal present. */
static std::pair<int, int> findQuietestRegion (const AudioData& a, double seconds)
{
    const int win = std::max (1024, (int) (seconds * a.sampleRate));
    const int n = a.numFrames();
    if (n <= win)
        return { 0, n };
    const int step = win / 4;
    double best = 1e30;
    int bestStart = 0;
    for (int s = 0; s + win <= n; s += step)
    {
        double acc = 0.0;
        for (const auto& ch : a.channels)
            for (int i = s; i < s + win; i += 2)
                acc += (double) ch[(size_t) i] * ch[(size_t) i];
        if (acc > 1e-12 && acc < best) // skip digital silence
        {
            best = acc;
            bestStart = s;
        }
    }
    return { bestStart, bestStart + win };
}

int main (int argc, char** argv)
{
    if (argc >= 2 && std::strcmp (argv[1], "--list-params") == 0)
    {
        for (const auto& p : paramTable())
            std::printf ("%-18s %8.2f .. %8.2f  default %8.2f %s\n", p.id, p.minValue, p.maxValue, p.defaultValue, p.unit);
        return 0;
    }
    if (argc < 3)
        return usage();

    const std::string inPath = argv[1], outPath = argv[2];
    std::string profilePath, scPath, err;
    std::vector<std::pair<int, float>> overrides;
    double learnStart = -1, learnEnd = -1;
    bool autoNoise = false, pcm24 = false;

    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&] () -> std::string {
            if (i + 1 >= argc) { std::fprintf (stderr, "missing value for %s\n", a.c_str()); std::exit (2); }
            return argv[++i];
        };
        if (a == "--profile") profilePath = next();
        else if (a == "--sidechain") scPath = next();
        else if (a == "--auto-noise") autoNoise = true;
        else if (a == "--pcm24") pcm24 = true;
        else if (a == "--learn-noise")
        {
            const auto v = next();
            if (std::sscanf (v.c_str(), "%lf:%lf", &learnStart, &learnEnd) != 2 || learnEnd <= learnStart)
            { std::fprintf (stderr, "bad --learn-noise range '%s'\n", v.c_str()); return 2; }
        }
        else if (a == "--set")
        {
            const auto v = next();
            const auto eq = v.find ('=');
            const int idx = eq == std::string::npos ? -1 : findParam (v.substr (0, eq));
            if (idx < 0) { std::fprintf (stderr, "unknown parameter in '%s' (see --list-params)\n", v.c_str()); return 2; }
            overrides.emplace_back (idx, std::strtof (v.c_str() + eq + 1, nullptr));
        }
        else
            return usage();
    }

    AudioData in, sc;
    if (! readWav (inPath, in, err)) { std::fprintf (stderr, "error: %s\n", err.c_str()); return 1; }
    const bool haveSc = ! scPath.empty();
    if (haveSc)
    {
        if (! readWav (scPath, sc, err)) { std::fprintf (stderr, "error: %s\n", err.c_str()); return 1; }
        if ((int) sc.sampleRate != (int) in.sampleRate)
        { std::fprintf (stderr, "error: side-chain sample rate %.0f differs from vocal %.0f\n", sc.sampleRate, in.sampleRate); return 1; }
    }

    const int numCh = std::min ((int) in.channels.size(), 8);
    const int blockSize = 512;
    auto engine = std::make_unique<StudioEngine>();
    engine->prepare (in.sampleRate, blockSize, numCh);

    if (! profilePath.empty())
    {
        Profile prof;
        if (! loadProfileFile (profilePath, prof, &err)) { std::fprintf (stderr, "error: profile: %s\n", err.c_str()); return 1; }
        for (auto [idx, v] : prof.values)
            engine->setParam (idx, v);
    }
    for (auto [idx, v] : overrides)
    {
        const auto& info = paramTable()[(size_t) idx];
        engine->setParam (idx, std::fmin (std::fmax (v, info.minValue), info.maxValue));
    }

    // --- Optional noise-profile learning pass ---------------------------------
    int ls = -1, le = -1;
    if (learnStart >= 0)
    {
        ls = (int) (learnStart * in.sampleRate);
        le = std::min (in.numFrames(), (int) (learnEnd * in.sampleRate));
    }
    else if (autoNoise)
    {
        auto r = findQuietestRegion (in, 0.5);
        ls = r.first;
        le = r.second;
    }
    if (ls >= 0 && le > ls)
    {
        auto& dn = engine->getDenoiser();
        dn.setLearning (true);
        std::vector<std::vector<float>> tmp ((size_t) numCh, std::vector<float> ((size_t) (le - ls)));
        std::vector<float*> ptrs;
        for (int c = 0; c < numCh; ++c)
        {
            std::copy (in.channels[(size_t) c].begin() + ls, in.channels[(size_t) c].begin() + le, tmp[(size_t) c].begin());
            ptrs.push_back (tmp[(size_t) c].data());
        }
        engine->process (ptrs.data(), numCh, le - ls);
        dn.setLearning (false);
        float dummy[1] { 0.0f };
        float* dp[1] { dummy };
        engine->process (dp, 1, 1); // commits the learned profile
        engine->reset();
        std::fprintf (stderr, "learned noise profile from %.2fs - %.2fs\n", ls / in.sampleRate, le / in.sampleRate);
    }

    // --- Render with latency compensation -------------------------------------
    const int latency = engine->getLatencySamples();
    const int n = in.numFrames();
    const int total = n + latency;
    std::vector<std::vector<float>> buf ((size_t) numCh, std::vector<float> ((size_t) total, 0.0f));
    for (int c = 0; c < numCh; ++c)
        std::copy (in.channels[(size_t) c].begin(), in.channels[(size_t) c].end(), buf[(size_t) c].begin());

    const int scCh = haveSc ? std::min ((int) sc.channels.size(), 2) : 0;
    std::vector<std::vector<float>> scBuf ((size_t) scCh, std::vector<float> ((size_t) total, 0.0f));
    for (int c = 0; c < scCh; ++c)
        std::copy (sc.channels[(size_t) c].begin(),
                   sc.channels[(size_t) c].begin() + std::min (sc.numFrames(), total),
                   scBuf[(size_t) c].begin());

    for (int pos = 0; pos < total; pos += blockSize)
    {
        const int len = std::min (blockSize, total - pos);
        float* ch[8] {};
        const float* scp[2] {};
        for (int c = 0; c < numCh; ++c)
            ch[c] = buf[(size_t) c].data() + pos;
        for (int c = 0; c < scCh; ++c)
            scp[c] = scBuf[(size_t) c].data() + pos;
        engine->process (ch, numCh, len, scCh > 0 ? scp : nullptr, scCh);
    }

    AudioData out;
    out.sampleRate = in.sampleRate;
    out.channels.assign ((size_t) numCh, std::vector<float> ((size_t) n));
    for (int c = 0; c < numCh; ++c)
        std::copy (buf[(size_t) c].begin() + latency, buf[(size_t) c].end(), out.channels[(size_t) c].begin());

    if (! writeWav (outPath, out, ! pcm24, err)) { std::fprintf (stderr, "error: %s\n", err.c_str()); return 1; }
    std::fprintf (stderr, "rendered %s (%d ch, %.0f Hz, latency %d samples compensated)\n",
                  outPath.c_str(), numCh, in.sampleRate, latency);
    return 0;
}
