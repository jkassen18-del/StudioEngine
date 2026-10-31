#pragma once
// Minimal RIFF/WAVE reader/writer (PCM 16/24/32-bit, IEEE float 32/64-bit,
// WAVE_FORMAT_EXTENSIBLE). Used by the command-line renderer only.
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace se
{
struct AudioData
{
    double sampleRate = 48000.0;
    std::vector<std::vector<float>> channels;
    int numFrames() const { return channels.empty() ? 0 : (int) channels[0].size(); }
};

inline bool readWav (const std::string& path, AudioData& out, std::string& err)
{
    std::ifstream f (path, std::ios::binary);
    if (! f) { err = "cannot open " + path; return false; }
    std::vector<char> bytes ((std::istreambuf_iterator<char> (f)), std::istreambuf_iterator<char>());
    auto u16 = [&] (size_t p) { uint16_t v; std::memcpy (&v, &bytes[p], 2); return v; };
    auto u32 = [&] (size_t p) { uint32_t v; std::memcpy (&v, &bytes[p], 4); return v; };

    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (&bytes[8], "WAVE", 4) != 0)
    { err = "not a RIFF/WAVE file: " + path; return false; }

    int format = 0, numCh = 0, bits = 0;
    uint32_t rate = 0;
    size_t dataPos = 0, dataLen = 0;
    size_t p = 12;
    while (p + 8 <= bytes.size())
    {
        const uint32_t len = u32 (p + 4);
        if (std::memcmp (&bytes[p], "fmt ", 4) == 0 && p + 24 <= bytes.size())
        {
            format = u16 (p + 8);
            numCh = u16 (p + 10);
            rate = u32 (p + 12);
            bits = u16 (p + 22);
            if (format == 0xFFFE && len >= 40)
                format = u16 (p + 8 + 24); // sub-format GUID first two bytes
        }
        else if (std::memcmp (&bytes[p], "data", 4) == 0)
        {
            dataPos = p + 8;
            dataLen = std::min<size_t> (len, bytes.size() - dataPos);
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (numCh <= 0 || dataPos == 0) { err = "missing fmt/data chunk"; return false; }
    if (! ((format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && (bits == 32 || bits == 64))))
    { err = "unsupported WAV encoding (format " + std::to_string (format) + ", " + std::to_string (bits) + " bit)"; return false; }

    const int bps = bits / 8;
    const size_t frames = dataLen / (size_t) (bps * numCh);
    out.sampleRate = rate;
    out.channels.assign ((size_t) numCh, std::vector<float> (frames));
    const char* d = bytes.data() + dataPos;
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < numCh; ++c)
        {
            const char* s = d + (i * (size_t) numCh + (size_t) c) * (size_t) bps;
            float v = 0.0f;
            if (format == 3 && bits == 32) { std::memcpy (&v, s, 4); }
            else if (format == 3) { double dv; std::memcpy (&dv, s, 8); v = (float) dv; }
            else if (bits == 16) { int16_t x; std::memcpy (&x, s, 2); v = x / 32768.0f; }
            else if (bits == 24)
            {
                int32_t x = (int32_t) ((uint32_t) (uint8_t) s[0] << 8 | (uint32_t) (uint8_t) s[1] << 16 | (uint32_t) (uint8_t) s[2] << 24);
                v = (float) (x / 2147483648.0);
            }
            else { int32_t x; std::memcpy (&x, s, 4); v = (float) (x / 2147483648.0); }
            out.channels[(size_t) c][i] = v;
        }
    return true;
}

/** Writes 32-bit float WAV (lossless for further analysis) or 24-bit PCM. */
inline bool writeWav (const std::string& path, const AudioData& a, bool float32, std::string& err)
{
    std::ofstream f (path, std::ios::binary);
    if (! f) { err = "cannot write " + path; return false; }
    const uint16_t numCh = (uint16_t) a.channels.size();
    const uint16_t bits = float32 ? 32 : 24;
    const uint32_t frames = (uint32_t) a.numFrames();
    const uint32_t dataLen = frames * numCh * (bits / 8);
    auto w16 = [&] (uint16_t v) { f.write ((const char*) &v, 2); };
    auto w32 = [&] (uint32_t v) { f.write ((const char*) &v, 4); };
    f.write ("RIFF", 4); w32 (36 + dataLen); f.write ("WAVE", 4);
    f.write ("fmt ", 4); w32 (16); w16 (float32 ? 3 : 1); w16 (numCh);
    w32 ((uint32_t) a.sampleRate); w32 ((uint32_t) a.sampleRate * numCh * (bits / 8));
    w16 ((uint16_t) (numCh * (bits / 8))); w16 (bits);
    f.write ("data", 4); w32 (dataLen);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < numCh; ++c)
        {
            const float v = a.channels[c][i];
            if (float32)
                f.write ((const char*) &v, 4);
            else
            {
                const double cl = v < -1.0f ? -1.0 : (v > 1.0f ? 1.0 : (double) v);
                const int32_t x = (int32_t) (cl * 8388607.0);
                const char b[3] { (char) (x & 0xff), (char) ((x >> 8) & 0xff), (char) ((x >> 16) & 0xff) };
                f.write (b, 3);
            }
        }
    return (bool) f;
}
} // namespace se
