// EffectDesc — a serializable description of one track effect.
//
// The model stays kit-free and owns no live DSP state: it stores an ordered
// list of these descriptors per track. The engine instantiates real IEffect
// objects from them (see dsp/EffectFactory) when it builds its graph. Params
// are generic slots interpreted per type, which keeps the model simple and
// trivially serializable later.
#pragma once

namespace daw {

enum class EffectType { Biquad, Delay };

struct EffectDesc {
    EffectType type = EffectType::Biquad;
    // Per-type param slots:
    //   Biquad: p0 = mode (0 LowPass, 1 HighPass, 2 Peaking), p1 = freq (Hz),
    //           p2 = Q, p3 = gain dB (peaking only)
    //   Delay:  p0 = time (s), p1 = feedback [0,1), p2 = mix [0,1]
    float p0 = 0.0f;
    float p1 = 0.0f;
    float p2 = 0.0f;
    float p3 = 0.0f;
};

// Convenience builders for the common presets the UI drops in.
inline EffectDesc LowPassDesc(float freq = 800.0f, float q = 0.707f) {
    return EffectDesc{EffectType::Biquad, 0.0f, freq, q, 0.0f};
}
inline EffectDesc HighPassDesc(float freq = 200.0f, float q = 0.707f) {
    return EffectDesc{EffectType::Biquad, 1.0f, freq, q, 0.0f};
}
inline EffectDesc DelayDesc(float sec = 0.25f, float fb = 0.35f, float mix = 0.3f) {
    return EffectDesc{EffectType::Delay, sec, fb, mix, 0.0f};
}

} // namespace daw
