// EffectDesc — a serializable description of one track effect.
//
// The model stays kit-free and owns no live DSP state: it stores an ordered
// list of these descriptors per track. The engine instantiates real IEffect
// objects from them (see dsp/EffectFactory) when it builds its graph. Params
// are a flat float vector whose meaning is per type, which keeps the model
// simple, trivially serializable, and lets effects carry as many params as
// they need (e.g. the parametric EQ's 15).
#pragma once

#include <initializer_list>
#include <utility>

#include <cstddef>
#include <string>
#include <vector>

namespace daw {

// Order is the serialized id (0..9); do not reorder without bumping the file
// format (see ProjectIO). Append new types at the end so existing ids stay put.
// Biquad is kept for loading older projects; new tone shaping uses the
// parametric Eq.
enum class EffectType { Biquad, Delay, Reverb, Compressor, Eq,
                        Saturator, Gate, Widener, Plugin, Limiter };

// Highest valid serialized EffectType id. Every place that validates a
// deserialized/message type (ProjectIO load, the MainWindow kMsgApplyFx handler)
// gates against this so a new type is never silently coerced to Biquad. Point it
// at the LAST enumerator — appending a type then updates every gate at once.
inline constexpr int kMaxEffectTypeId = static_cast<int>(EffectType::Limiter);

struct EffectDesc {
    EffectType         type = EffectType::Biquad;
    std::vector<float> params;
    std::string        pluginName;   // set when type == Plugin (add-on id)
    // Per-type param layout:
    //   Biquad:     [mode(0 LP,1 HP,2 Peak), freq Hz, Q, gain dB]
    //   Delay:      [time s, feedback [0,1), mix [0,1], sync 0/1, division idx]
    //   Reverb:     [roomSize [0,1], mix [0,1]]
    //   Compressor: [threshold dB, ratio, attack ms, release ms, makeup dB]
    //   Eq:         5 bands of [freq Hz, gain dB, Q] (band 0 = low shelf,
    //               bands 1-3 = peaks, band 4 = high shelf)
    //   Saturator:  [drive [0,1], mix [0,1], output trim dB]
    //   Gate:       [threshold dB, ratio, attack ms, release ms, range dB]
    //   Widener:    [width [0,2], pan [-1,1], gain]
    //   Limiter:    [ceiling dB, lookahead ms, release ms, input gain dB]

    // Read a param with a safe default for missing slots.
    float p(size_t i) const { return i < params.size() ? params[i] : 0.0f; }
};

// Convenience builders for the presets the UI drops in.
inline EffectDesc BiquadDesc(float mode, float freq, float q = 0.707f,
                             float gainDb = 0.0f) {
    return EffectDesc{EffectType::Biquad, {mode, freq, q, gainDb}};
}
inline EffectDesc LowPassDesc(float freq = 800.0f, float q = 0.707f) {
    return BiquadDesc(0.0f, freq, q);
}
inline EffectDesc HighPassDesc(float freq = 200.0f, float q = 0.707f) {
    return BiquadDesc(1.0f, freq, q);
}
inline EffectDesc DelayDesc(float sec = 0.25f, float fb = 0.35f, float mix = 0.3f,
                            float sync = 0.0f, float division = 0.0f) {
    return EffectDesc{EffectType::Delay, {sec, fb, mix, sync, division}};
}
inline EffectDesc ReverbDesc(float roomSize = 0.5f, float mix = 0.3f) {
    return EffectDesc{EffectType::Reverb, {roomSize, mix}};
}
// DuskVerb engine presets: params [size, mix, algorithm, decay s, tone].
inline EffectDesc DuskPlateDesc() {
    return EffectDesc{EffectType::Reverb, {0.7f, 0.35f, 1.0f, 2.6f, 0.5f}};
}
inline EffectDesc DuskHallDesc() {
    return EffectDesc{EffectType::Reverb, {0.7f, 0.35f, 2.0f, 3.4f, 0.5f}};
}
inline EffectDesc DuskFdnDesc() {
    return EffectDesc{EffectType::Reverb, {0.7f, 0.35f, 3.0f, 2.8f, 0.5f}};
}
inline EffectDesc CompressorDesc(float thrDb = -20.0f, float ratio = 4.0f,
                                 float attackMs = 10.0f, float releaseMs = 100.0f,
                                 float makeupDb = 0.0f) {
    return EffectDesc{EffectType::Compressor,
                      {thrDb, ratio, attackMs, releaseMs, makeupDb}};
}
inline EffectDesc SaturatorDesc(float drive = 0.5f, float mix = 1.0f,
                                float trimDb = 0.0f) {
    return EffectDesc{EffectType::Saturator, {drive, mix, trimDb}};
}
inline EffectDesc GateDesc(float thrDb = -40.0f, float ratio = 4.0f,
                           float attackMs = 1.0f, float releaseMs = 100.0f,
                           float rangeDb = 60.0f) {
    return EffectDesc{EffectType::Gate,
                      {thrDb, ratio, attackMs, releaseMs, rangeDb}};
}
inline EffectDesc WidenerDesc(float width = 1.0f, float pan = 0.0f,
                              float gain = 1.0f) {
    return EffectDesc{EffectType::Widener, {width, pan, gain}};
}
// Live look-ahead brickwall limiter — the one built-in effect with non-zero
// reported latency (== lookaheadMs), so it exercises plugin-delay compensation.
inline EffectDesc LimiterDesc(float ceilingDb = -1.0f, float lookaheadMs = 5.0f,
                              float releaseMs = 60.0f, float inGainDb = 0.0f) {
    return EffectDesc{EffectType::Limiter,
                      {ceilingDb, lookaheadMs, releaseMs, inGainDb}};
}
// Default parametric EQ: low shelf, three peaks, high shelf, all flat.
inline EffectDesc EqDesc() {
    return EffectDesc{EffectType::Eq, {
        80.0f,   0.0f, 0.70f,
        240.0f,  0.0f, 0.90f,
        750.0f,  0.0f, 0.90f,
        2200.0f, 0.0f, 0.90f,
        6500.0f, 0.0f, 0.70f }};
}

// Value range of effect parameter `slot` (same slot order as the editor knobs).
// Used by the automation UI to map a lane value <-> pixels. Mirrors the editor
// knob ranges. Unknown slots default to [0, 1].
inline void FxParamRange(EffectType t, int slot, float* mn, float* mx) {
    float lo = 0.0f, hi = 1.0f;
    auto pick = [&](std::initializer_list<std::pair<float, float>> r) {
        int i = 0;
        for (auto& p : r) { if (i == slot) { lo = p.first; hi = p.second; } i++; }
    };
    switch (t) {
        case EffectType::Delay:      pick({{0.01f,1},{0,0.95f},{0,1},{0,1},{0,6}}); break;
        case EffectType::Reverb:     pick({{0,1},{0,1},{0,3},{0.2f,12},{0,1}}); break;
        case EffectType::Compressor: pick({{-60,0},{1,20},{0.1f,100},{5,1000},{0,24}}); break;
        case EffectType::Saturator:  pick({{0,1},{0,1},{-24,24}}); break;
        case EffectType::Gate:       pick({{-80,0},{1,20},{0.1f,100},{5,1000},{0,80}}); break;
        case EffectType::Widener:    pick({{0,2},{-1,1},{0,2}}); break;
        case EffectType::Limiter:    pick({{-24,0},{0.1f,20},{1,2000},{0,24}}); break;
        case EffectType::Eq: {
            const int w = slot % 3;
            if (w == 0) { lo = 20; hi = 18000; }
            else if (w == 1) { lo = -18; hi = 18; }
            else { lo = 0.3f; hi = 8; }
            break;
        }
        case EffectType::Biquad:
        default:
            if (slot == 1) { lo = 20; hi = 16000; }
            else if (slot == 2) { lo = 0.1f; hi = 10; }
            else { lo = -24; hi = 24; }
            break;
    }
    if (mn) *mn = lo;
    if (mx) *mx = hi;
}

} // namespace daw
