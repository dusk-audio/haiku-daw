// Biquad — a second-order IIR filter (RBJ cookbook), one of the built-in
// effects. Low-pass / high-pass / peaking-EQ modes cover the common track
// tone-shaping needs with one small, well-understood building block.
//
// Direct Form I, independent state per stereo channel. Coefficients are
// recomputed in Prepare() (and whenever a parameter setter is called before
// Prepare); Process() is pure arithmetic on preallocated state.
//
// Kit-free, host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Biquad : public IEffect {
public:
    enum class Type { LowPass, HighPass, Peaking };

    Biquad(Type type = Type::LowPass, double freq = 1000.0,
           double q = 0.707, double gainDb = 0.0);

    void SetParams(Type type, double freq, double q, double gainDb);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;   // 0 mode,1 freq,2 Q,3 gainDb
    const char* Name() const override { return "Biquad"; }

private:
    void Recompute();    // -> target coefficients
    void SnapCoeffs();   // live = target (no glide), for Prepare/construct

    Type   fType;
    double fFreq;
    double fQ;
    double fGainDb;
    double fSampleRate = 48000.0;

    // Live normalized coefficients (a0 folded in) used by Process, and the
    // targets they glide toward. SetParam/automation moves the targets; Process
    // one-pole-smooths the live coeffs to them per sample so a coefficient step
    // at a block boundary can't inject a click (zipper noise). When converged
    // (no automation) live == target, so the glide is a no-op and the static
    // response is unchanged.
    double fB0 = 1, fB1 = 0, fB2 = 0, fA1 = 0, fA2 = 0;
    double fTB0 = 1, fTB1 = 0, fTB2 = 0, fTA1 = 0, fTA2 = 0;
    double fCoefSmooth = 0.0;   // per-sample glide factor (0 = snap), set in Prepare

    // Per-channel Direct Form I state: x[n-1], x[n-2], y[n-1], y[n-2].
    double fX1[2] = {0, 0}, fX2[2] = {0, 0};
    double fY1[2] = {0, 0}, fY2[2] = {0, 0};
};

} // namespace daw
