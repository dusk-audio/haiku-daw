// Fade — the shape of a clip fade, and the one place the curve is defined.
//
// Kit-free and header-only: the engine's fade envelope (RT), the exporter's
// (offline) and the timeline's drawn ramp all call FadeGainAt, so the shape the
// user picks is the shape that sounds and the shape that is drawn. A fade's
// shape is chosen per clip (Clip::fadeShape); an auto-crossfade's contribution
// renders equal-power, which is the DAW default for a crossfade (the two ramps
// then sum to constant power: in^2 + out^2 = 1).
#pragma once

#include <cmath>

namespace daw {

enum class FadeShape {
    Linear = 0,       // gain = t (the editor's historical behaviour, default)
    EqualPower = 1,   // gain = sin(t*pi/2): constant power in a crossfade
    SCurve = 2,       // gain = t*t*(3-2t): smoothstep, gentle at both ends
};

inline const char* FadeShapeName(FadeShape s) {
    switch (s) {
        case FadeShape::EqualPower: return "Equal Power";
        case FadeShape::SCurve:     return "S-Curve";
        default:                    return "Linear";
    }
}

// The stored form (ProjectIO's `clipinfo` line): an int, like every other enum
// in the format. An unknown value (a newer build's shape) clamps to Linear —
// the fade still renders, it just renders as the default curve.
inline int FadeShapeToInt(FadeShape s) { return (int)s; }

inline FadeShape FadeShapeFromInt(int i) {
    switch (i) {
        case 1:  return FadeShape::EqualPower;
        case 2:  return FadeShape::SCurve;
        default: return FadeShape::Linear;
    }
}

// The envelope multiplier at fraction `t` through a fade: 0 at the silent edge
// (the START of a fade-in, the END of a fade-out), 1 at full level. Both
// directions use the same curve, so a fade-in meets the material and a fade-out
// leaves it along the same shape. t <= 0 / t >= 1 are exact so a renderer can
// call this on every sample without an epsilon.
inline float FadeGainAt(FadeShape shape, float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    switch (shape) {
        case FadeShape::EqualPower:
            // sin(pi/2 * t): pairs with a mirrored ramp to in^2+out^2 = 1.
            return std::sin(t * 1.5707963267948966f);
        case FadeShape::SCurve:
            return t * t * (3.0f - 2.0f * t);   // smoothstep
        default:
            return t;
    }
}

} // namespace daw
