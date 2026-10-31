# Studio Engine

**Studio Engine** is a vocal clean-up and mastering plugin (VST3 / AU / Standalone).
It removes background noise from a recording, then mixes and masters the vocal so it
sits on your instrumental. It comes with a **reference matcher**: give it mastered
acapellas whose sound you like, and it works out the plugin settings that make your
vocal match them as closely as possible.

```
Input ─► Low Cut ─► Spectral Denoise ─► Gate ─► Match EQ ─► De-ess ─► Compressor
      ─► Tone EQ ─► Mix Fit (← instrumental side-chain) ─► Warmth ─► Limiter ─► Output
```

| Stage | What it does |
|---|---|
| **Low Cut** | 24 dB/oct high-pass that removes rumble, handling noise and plosive thumps. |
| **Spectral Denoise** | STFT noise reduction. **Learn Noise** captures a noise print from a few seconds of room tone. **Adaptive** mode tracks changing noise (fans, AC, hiss, hum) automatically using minimum statistics. A decision-directed Wiener gain plus spectral smoothing keeps the voice natural and avoids "musical noise" artefacts. |
| **Gate** | A soft downward expander with hysteresis and hold. It cleans the gaps between phrases without chopping word endings. |
| **Match EQ** | 10 octave bands (31 Hz–16 kHz). The reference matcher sets these to give your vocal the reference's tonal balance. |
| **De-esser** | A dynamic high shelf that only pulls down harsh "s"/"t" energy while it's present. At rest it is a perfect bypass. |
| **Compressor** | Soft-knee vocal compressor (RMS/peak blend) with makeup gain and parallel mix. |
| **Tone EQ** | Four musical controls: Low, Mud, Presence and Air. |
| **Mix Fit** | Pairs the vocal to the **instrumental on the side-chain input**. It rides the vocal level to keep a fixed loudness offset against the beat, and adds a dynamic presence lift wherever the beat masks the vocal. It stays neutral when nothing is connected. |
| **Warmth** | Gentle asymmetric tape-style saturation, adjustable from subtle to heavy. |
| **Limiter** | Look-ahead brickwall limiter. **Loudness** pushes into it and **Ceiling** sets the peak level. |

Meters show input/output loudness (LUFS) and the gain reduction of every stage.
Total latency is about 44 ms at 48 kHz and is reported to the host, so it is compensated automatically.

---

## Getting the plugin

### Option A: download a build (no compiler needed)
Every push to GitHub builds Windows, macOS and Linux versions automatically
(**Actions → Build Studio Engine → Artifacts**). Each download contains:

* `VST3/Studio Engine.vst3` → copy to
  * Windows: `C:\Program Files\Common Files\VST3\`
  * macOS: `~/Library/Audio/Plug-Ins/VST3/`
  * Linux: `~/.vst3/`
* `AU/Studio Engine.component` (macOS only) → `~/Library/Audio/Plug-Ins/Components/`
* `Standalone/`: runs without a DAW
* `studio_engine_cli` and `reference_match/`: the matching tools

> macOS builds are unsigned. If macOS blocks the plugin, run
> `xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Studio\ Engine.vst3`.

### Option B: build it yourself
Requirements: CMake 3.22+, a C++17 compiler (Visual Studio 2022, Xcode 14+, or GCC 11+), and git.
JUCE is downloaded automatically.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release      # DSP unit tests
```

The plugins end up in `build/StudioEngine_artefacts/Release/`.
Linux needs: `libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxcomposite-dev libfreetype-dev libfontconfig1-dev`.

---

## Using it in your DAW

1. Put **Studio Engine** on the **vocal track**, first in the chain.
2. **Clean the noise.** Find a spot where only room noise plays (before the first line is ideal).
   Press **Learn Noise**, play 2–3 seconds of it, then press the button again. Turn **Denoise** up until the
   background disappears. Leave **Adaptive** on if the noise changes over time.
   For very noisy takes, raise **Sensitivity**. If the voice starts sounding watery, lower it.
3. **Pair it with the instrumental.** Route the beat to the plugin's **side-chain input**
   ("Instrumental" bus). The Mix Fit panel says *Instrumental connected* once it's working.
   * **Ableton Live**: open the plugin's side-chain section and choose the beat track.
   * **FL Studio**: route the beat's mixer track to the vocal track with *Sidechain to this track*,
     then pick it as input 2 in the plugin's wrapper.
   * **Logic Pro**: choose the beat track in the plugin header's *Side Chain* menu.
   * **Studio One / Reaper / Cubase**: enable the plugin's side-chain and send the beat track to it.
4. Load a **profile** (**Load Profile**). Start with one from `profiles/` or use your own matched profile (below).
   Fine-tune by ear, then **Save Profile**.

The noise print is saved with your session.

---

## Matching your vocal to reference acapellas

This is the "make my vocal sound like *that*" workflow. You need Python 3 with `numpy`
(`pip install numpy`).

**1. Build a reference target** from one or more mastered acapellas
(see [`references/README.md`](references/README.md) for how to prepare them):

```bash
python tools/reference_match/studio_match.py analyze \
    references/my_sound/song1_acapella.wav references/my_sound/song2_acapella.wav \
    --beats references/my_sound/song1_instrumental.wav references/my_sound/song2_instrumental.wav \
    -o profiles/targets/my_sound.json
```

The analysis measures:

* integrated loudness (BS.1770 LUFS)
* true peak
* noise floor and SNR
* dynamic range (phrase-level spread)
* crest factor
* 1/3-octave tonal balance
* spectral tilt
* sibilance
* stereo width

With `--beats` it also measures how loud the vocal sits against its instrumental.

**2. Match your raw vocal** to that target:

```bash
python tools/reference_match/studio_match.py match \
    --reference profiles/targets/my_sound.json \
    --vocal my_raw_vocal.wav --beat my_instrumental.wav \
    -o profiles/my_song.json --render my_vocal_matched.wav
```

The matcher picks starting settings, then runs a **closed loop**:

1. It renders your vocal through the real Studio Engine DSP (`studio_engine_cli`).
2. It re-measures the result against the reference.
3. It refines the Match EQ (least-squares fit), compressor, de-esser, noise reduction and limiter.
4. It repeats this and keeps the closest result.

Each pass reports its progress:

```
iter 3: spectrum 1.8 dB rms | loudness -0.09 LU | dynamics +0.18 dB | sibilance +0.4 dB | peak +0.07 dB
```

**3. Load `profiles/my_song.json`** in the plugin with **Load Profile**, and keep the
instrumental on the side-chain.

The matcher protects the sound in two ways:

* It never boosts a band past the recording's own noise floor, so matching can't pull hiss up.
* If you give it your instrumental, the vocal's target loudness follows how the reference vocal sat against *its* beat.

Other commands:

```bash
python tools/reference_match/studio_match.py report  file.wav                 # full analysis
python tools/reference_match/studio_match.py compare mine.wav reference.wav   # how close am I?
```

### Offline renderer
`studio_engine_cli` runs the exact same processing as the plugin on WAV files:

```bash
studio_engine_cli in.wav out.wav --profile profiles/my_song.json --sidechain beat.wav --auto-noise
studio_engine_cli --list-params
```

---

## Project layout

```
Source/dsp/          framework-free DSP (shared by plugin, CLI and tests)
Source/plugin/       JUCE processor + editor
tools/cli/           offline renderer (studio_engine_cli)
tools/reference_match/studio_match.py   reference analyser / matcher
profiles/            starter profiles (+ your matched ones)
references/          drop reference acapellas here
tests/               DSP unit tests and synthetic test-audio generator
```
