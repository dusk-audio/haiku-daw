// Widener — a stereo width / utility effect, one of the built-in effects.
// Mid/Side processing: split the signal into a mono "mid" (L+R)/2 and a
// "side" (L-R)/2, scale the side by `width` (0 = mono, 1 = unchanged,
// up to ~2 = extra wide), then reconstruct. A trailing equal-power pan and
// an output gain make it a general stereo-utility node.
//
//   mid  = (L + R) / 2
//   side = (L - R) / 2
//   L'   = (mid + side*width) * panL * gain
//   R'   = (mid - side*width) * panR * gain
//
// No filter state, so Reset() is a no-op; Process() is pure per-sample
// arithmetic on preallocated coefficients (RT-safe, no allocation).
//
// Kit-free, host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Widener : public IEffect {
public:
    // width [0..2] (0 mono, 1 unity, 2 extra-wide), pan [-1..+1]
    // (-1 hard left, 0 center, +1 hard right), gain linear output trim.
    Widener(double width = 1.0, double pan = 0.0, double gain = 1.0);

    void SetParams(double width, double pan, double gain = 1.0);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    const char* Name() const override { return "Widener"; }

private:
    void Recompute();

    double fWidth;
    double fPan;
    double fGain;

    // Precomputed per-channel pan*gain multipliers (equal-power, normalized so
    // pan=0 -> unity on both channels).
    double fPanL = 1.0;
    double fPanR = 1.0;
};

} // namespace daw
