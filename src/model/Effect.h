// EffectDesc — a serializable description of one track effect.
//
// The model stays kit-free and owns no live DSP state: it stores an ordered
// list of these descriptors per track. The engine instantiates real IEffect
// objects from them (see dsp/EffectFactory) when it builds its graph. Params
// are generic slots interpreted per type, which keeps the model simple and
// trivially serializable later.
#pragma once

namespace daw {

// Order is the serialized id (0..3); do not reorder without bumping the file
// format (see ProjectIO).
enum class EffectType { Biquad, Delay, Reverb, Compressor };

struct EffectDesc {
    EffectType type = EffectType::Biquad;
    // Per-type param slots:
    //   Biquad:     p0 = mode (0 LowPass, 1 HighPass, 2 Peaking), p1 = freq Hz,
    //               p2 = Q, p3 = gain dB (peaking only)
    //   Delay:      p0 = time s, p1 = feedback [0,1), p2 = mix [0,1]
    //   Reverb:     p0 = roomSize [0,1], p1 = mix [0,1]
    //   Compressor: p0 = threshold dB, p1 = ratio, p2 = attack ms, p3 = release ms
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
inline EffectDesc ReverbDesc(float roomSize = 0.5f, float mix = 0.3f) {
    return EffectDesc{EffectType::Reverb, roomSize, mix, 0.0f, 0.0f};
}
inline EffectDesc CompressorDesc(float thrDb = -20.0f, float ratio = 4.0f,
                                 float attackMs = 10.0f, float releaseMs = 100.0f) {
    return EffectDesc{EffectType::Compressor, thrDb, ratio, attackMs, releaseMs};
}

} // namespace daw
