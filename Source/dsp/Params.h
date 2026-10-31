#pragma once
// Studio Engine - parameter table shared by the plugin, the CLI renderer and
// the reference-match profiles (JSON). Parameter IDs are stable: profiles and
// host automation refer to them.
#include <array>
#include <string>

namespace se
{
enum Param : int
{
    pInputGain,
    pHpfOn, pHpfFreq,
    pDnOn, pDnAmount, pDnSensitivity, pDnAdaptive,
    pGateOn, pGateThreshold, pGateRange, pGateAttack, pGateRelease,
    pMatchOn, pMatchAmount,
    pMatch0, pMatch1, pMatch2, pMatch3, pMatch4, pMatch5, pMatch6, pMatch7, pMatch8, pMatch9,
    pDeessOn, pDeessFreq, pDeessThreshold, pDeessRange,
    pCompOn, pCompThreshold, pCompRatio, pCompAttack, pCompRelease, pCompKnee, pCompMakeup, pCompMix,
    pEqOn, pEqLowFreq, pEqLowGain, pEqMudFreq, pEqMudGain, pEqPresenceFreq, pEqPresenceGain, pEqAirFreq, pEqAirGain,
    pMixOn, pMixOffset, pMixRange, pMixUnmask,
    pSatOn, pSatDrive, pSatMix,
    pLimOn, pLimDrive, pLimCeiling, pLimRelease,
    pOutputGain,
    kNumParams
};

struct ParamInfo
{
    const char* id;
    const char* name;
    float minValue, maxValue, defaultValue;
    float skewCentre; // 0 = linear, otherwise value at the slider's centre
    const char* unit;
    bool isToggle;
};

inline const std::array<ParamInfo, kNumParams>& paramTable()
{
    static const std::array<ParamInfo, kNumParams> t { {
        { "input_gain", "Input Gain", -24.f, 24.f, 0.f, 0.f, "dB", false },

        { "hpf_on", "HPF On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "hpf_freq", "HPF Freq", 20.f, 400.f, 80.f, 90.f, "Hz", false },

        { "dn_on", "Denoise On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "dn_amount", "Denoise Amount", 0.f, 40.f, 18.f, 0.f, "dB", false },
        { "dn_sensitivity", "Denoise Sensitivity", -10.f, 10.f, 0.f, 0.f, "dB", false },
        { "dn_adaptive", "Denoise Adaptive", 0.f, 1.f, 1.f, 0.f, "", true },

        { "gate_on", "Gate On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "gate_threshold", "Gate Threshold", -90.f, 0.f, -55.f, 0.f, "dB", false },
        { "gate_range", "Gate Range", 0.f, 80.f, 15.f, 0.f, "dB", false },
        { "gate_attack", "Gate Attack", 0.1f, 50.f, 2.f, 5.f, "ms", false },
        { "gate_release", "Gate Release", 10.f, 1000.f, 150.f, 150.f, "ms", false },

        { "match_on", "Match EQ On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "match_amount", "Match Amount", 0.f, 150.f, 100.f, 0.f, "%", false },
        { "match_31", "Match 31 Hz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_62", "Match 62 Hz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_125", "Match 125 Hz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_250", "Match 250 Hz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_500", "Match 500 Hz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_1k", "Match 1 kHz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_2k", "Match 2 kHz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_4k", "Match 4 kHz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_8k", "Match 8 kHz", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "match_16k", "Match 16 kHz", -12.f, 12.f, 0.f, 0.f, "dB", false },

        { "deess_on", "De-Ess On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "deess_freq", "De-Ess Freq", 3000.f, 12000.f, 6500.f, 6000.f, "Hz", false },
        { "deess_threshold", "De-Ess Threshold", -60.f, 0.f, -30.f, 0.f, "dB", false },
        { "deess_range", "De-Ess Range", 0.f, 24.f, 8.f, 0.f, "dB", false },

        { "comp_on", "Comp On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "comp_threshold", "Comp Threshold", -60.f, 0.f, -22.f, 0.f, "dB", false },
        { "comp_ratio", "Comp Ratio", 1.f, 20.f, 3.f, 4.f, ":1", false },
        { "comp_attack", "Comp Attack", 0.1f, 100.f, 8.f, 10.f, "ms", false },
        { "comp_release", "Comp Release", 10.f, 1000.f, 120.f, 150.f, "ms", false },
        { "comp_knee", "Comp Knee", 0.f, 24.f, 6.f, 0.f, "dB", false },
        { "comp_makeup", "Comp Makeup", -12.f, 24.f, 0.f, 0.f, "dB", false },
        { "comp_mix", "Comp Mix", 0.f, 100.f, 100.f, 0.f, "%", false },

        { "eq_on", "Tone EQ On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "eq_low_freq", "Low Freq", 40.f, 400.f, 120.f, 120.f, "Hz", false },
        { "eq_low_gain", "Low Gain", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "eq_mud_freq", "Mud Freq", 150.f, 1000.f, 350.f, 350.f, "Hz", false },
        { "eq_mud_gain", "Mud Gain", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "eq_presence_freq", "Presence Freq", 1000.f, 8000.f, 3000.f, 3000.f, "Hz", false },
        { "eq_presence_gain", "Presence Gain", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "eq_air_freq", "Air Freq", 6000.f, 18000.f, 10000.f, 10000.f, "Hz", false },
        { "eq_air_gain", "Air Gain", -12.f, 12.f, 0.f, 0.f, "dB", false },

        { "mix_on", "Mix Fit On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "mix_offset", "Vocal vs Beat", -12.f, 12.f, 0.f, 0.f, "dB", false },
        { "mix_range", "Ride Range", 0.f, 12.f, 6.f, 0.f, "dB", false },
        { "mix_unmask", "Unmask", 0.f, 9.f, 3.f, 0.f, "dB", false },

        { "sat_on", "Saturation On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "sat_drive", "Saturation Drive", 0.f, 24.f, 4.f, 0.f, "dB", false },
        { "sat_mix", "Saturation Mix", 0.f, 100.f, 25.f, 0.f, "%", false },

        { "lim_on", "Limiter On", 0.f, 1.f, 1.f, 0.f, "", true },
        { "lim_drive", "Limiter Drive", 0.f, 24.f, 0.f, 0.f, "dB", false },
        { "lim_ceiling", "Limiter Ceiling", -12.f, 0.f, -1.f, 0.f, "dB", false },
        { "lim_release", "Limiter Release", 5.f, 500.f, 80.f, 80.f, "ms", false },

        { "output_gain", "Output Gain", -24.f, 24.f, 0.f, 0.f, "dB", false },
    } };
    return t;
}

inline int findParam (const std::string& id)
{
    const auto& t = paramTable();
    for (int i = 0; i < kNumParams; ++i)
        if (id == t[(size_t) i].id)
            return i;
    return -1;
}

inline std::array<float, kNumParams> defaultParams()
{
    std::array<float, kNumParams> v {};
    for (int i = 0; i < kNumParams; ++i)
        v[(size_t) i] = paramTable()[(size_t) i].defaultValue;
    return v;
}

} // namespace se
