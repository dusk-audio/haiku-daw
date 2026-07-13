// Eq — a 5-band EQ (low shelf, 3 parametric peaks, high shelf), one of the
// built-in effects.
//
// Fixed band frequencies + Q; the five parameters (p0..p4) are the per-band
// gains in dB, which fits the EffectDesc's five slots. Coefficient math is
// ported from the user's Multi-Q AnalogMatchedBiquad ("amb") core
// (bandwidth-prewarped shelves/peaks, NOT RBJ-alpha) so the tone matches their
// plugin. Direct Form I, independent state per stereo channel; coefficients are
// recomputed in Prepare(), Process() is arithmetic on preallocated state.
//
// Kit-free (STL only), host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Eq : public IEffect {
public:
    static constexpr int kBands = 5;

    // Per-band gain in dB (0 = flat). Bands: 80 Hz shelf, 240 / 750 / 2200 Hz
    // peaks, 6.5 kHz shelf.
    Eq(float g0 = 0, float g1 = 0, float g2 = 0, float g3 = 0, float g4 = 0);

    void SetGains(float g0, float g1, float g2, float g3, float g4);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    const char* Name() const override { return "EQ"; }

private:
    void ComputeCoeffs();

    double fSampleRate = 48000.0;
    float  fGainDb[kBands];

    // Normalized biquad coefficients per band (a0 folded in).
    double fB0[kBands], fB1[kBands], fB2[kBands], fA1[kBands], fA2[kBands];

    // Per-band Direct Form I state: [band][channel] x[n-1],x[n-2],y[n-1],y[n-2].
    double fX1[kBands][2], fX2[kBands][2], fY1[kBands][2], fY2[kBands][2];
};

} // namespace daw
