// EffectDesc — a serializable description of one track effect.
//
// The model stays kit-free and owns no live DSP state: it stores an ordered
// list of these descriptors per track. The engine instantiates real IEffect
// objects from them (see dsp/EffectFactory) when it builds its graph. Params
// are a flat float vector whose meaning is per type, which keeps the model
// simple, trivially serializable, and lets effects carry as many params as
// they need (e.g. the parametric EQ's 15).
#pragma once

#include <cstddef>
#include <vector>

namespace daw {

// Order is the serialized id (0..7); do not reorder without bumping the file
// format (see ProjectIO). Biquad is kept for loading older projects; new tone
// shaping uses the parametric Eq.
enum class EffectType { Biquad, Delay, Reverb, Compressor, Eq,
                        Saturator, Gate, Widener };

struct EffectDesc {
    EffectType         type = EffectType::Biquad;
    std::vector<float> params;
    // Per-type param layout:
    //   Biquad:     [mode(0 LP,1 HP,2 Peak), freq Hz, Q, gain dB]
    //   Delay:      [time s, feedback [0,1), mix [0,1]]
    //   Reverb:     [roomSize [0,1], mix [0,1]]
    //   Compressor: [threshold dB, ratio, attack ms, release ms, makeup dB]
    //   Eq:         5 bands of [freq Hz, gain dB, Q] (band 0 = low shelf,
    //               bands 1-3 = peaks, band 4 = high shelf)
    //   Saturator:  [drive [0,1], mix [0,1], output trim dB]
    //   Gate:       [threshold dB, ratio, attack ms, release ms, range dB]
    //   Widener:    [width [0,2], pan [-1,1], gain]

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
inline EffectDesc DelayDesc(float sec = 0.25f, float fb = 0.35f, float mix = 0.3f) {
    return EffectDesc{EffectType::Delay, {sec, fb, mix}};
}
inline EffectDesc ReverbDesc(float roomSize = 0.5f, float mix = 0.3f) {
    return EffectDesc{EffectType::Reverb, {roomSize, mix}};
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
// Default parametric EQ: low shelf, three peaks, high shelf, all flat.
inline EffectDesc EqDesc() {
    return EffectDesc{EffectType::Eq, {
        80.0f,   0.0f, 0.70f,
        240.0f,  0.0f, 0.90f,
        750.0f,  0.0f, 0.90f,
        2200.0f, 0.0f, 0.90f,
        6500.0f, 0.0f, 0.70f }};
}

} // namespace daw
