// Gate — a peak-detecting downward expander / noise gate, one of the built-in
// effects.
//
// The dynamics mirror-image of Compressor: same stereo-linked peak detector and
// same gain-smoothing topology (the multi-comp donor's exp(-1/(t*Fs)) attack /
// release coefficients), but an opposite gain computer. Above threshold the
// signal passes at unity; BELOW threshold it is attenuated following the ratio
// (reduction dB = (threshold - level) * (ratio - 1)), clamped to a maximum of
// `range` dB. A ratio just above 1 is a gentle downward expander; a large ratio
// slams straight to the range floor and behaves like a hard gate.
//
// The smoothed GAIN opens with the attack coefficient (gain rising toward 1 as
// the signal crosses above threshold) and closes with the release coefficient
// (gain falling toward the range floor) — the gate convention, opposite to the
// compressor's. Coefficients are derived from the sample rate in Prepare();
// Process() is pure arithmetic on preallocated state.
//
// Kit-free (STL only), host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Gate : public IEffect {
public:
    // thresholdDb  = level below which attenuation starts (e.g. -40)
    // ratio        = expansion ratio, >= 1 (large => hard gate)
    // attackMs     = gate-open (gain-rise) time
    // releaseMs    = gate-close (gain-fall) time
    // rangeDb      = maximum attenuation when fully closed (e.g. 60)
    Gate(double thresholdDb = -40.0, double ratio = 4.0,
         double attackMs = 1.0, double releaseMs = 100.0,
         double rangeDb = 60.0);

    void SetParams(double thresholdDb, double ratio,
                   double attackMs, double releaseMs, double rangeDb);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;
    const char* Name() const override { return "Gate"; }

private:
    void Recompute();

    double fThresholdDb;
    double fRatio;
    double fAttackMs;
    double fReleaseMs;
    double fRangeDb;
    double fSampleRate = 48000.0;

    // Smoothing coefficients (per-sample) and cached linear range floor.
    double fAttackCoef  = 0.0;
    double fReleaseCoef = 0.0;
    double fRangeFloor  = 0.001;   // target 10^(-rangeDb/20), lowest gain closed
    double fFloorCur    = 0.001;   // live range floor, glided toward fRangeFloor
    double fCoefSmooth  = 0.0;     // per-sample glide factor (0 = snap), set in Prepare

    // Stereo-linked smoothed GAIN envelope (linear, 1 = fully open).
    double fEnv = 1.0;
};

} // namespace daw
