// Compressor — a peak-detecting dynamics processor, one of the built-in
// effects.
//
// Ported from the user's multi-comp DigitalCompressor (kit-free): a
// stereo-linked peak detector computes the dB overshoot above threshold, the
// ratio gives a target gain, and that GAIN is one-pole smoothed with the
// attack coefficient when it must fall and the release coefficient when it
// recovers (the donor's gain-smoothing topology, not level smoothing). Makeup
// gain is applied last. Attack/release smoothing coefficients
// (exp(-1/(t*Fs))) are derived from the sample rate in Prepare(); Process() is
// pure arithmetic on preallocated state.
//
// Kit-free (STL only), host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Compressor : public IEffect {
public:
    // thresholdDb  = level above which reduction starts (e.g. -20)
    // ratio        = input:output above threshold, >= 1 (e.g. 4 for 4:1)
    // attackMs     = envelope rise time
    // releaseMs    = envelope fall time
    // makeupDb     = output gain applied after compression
    Compressor(double thresholdDb = -20.0, double ratio = 4.0,
               double attackMs = 10.0, double releaseMs = 100.0,
               double makeupDb = 0.0);

    void SetParams(double thresholdDb, double ratio,
                   double attackMs, double releaseMs, double makeupDb);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;
    const char* Name() const override { return "Compressor"; }

private:
    void Recompute();

    double fThresholdDb;
    double fRatio;
    double fAttackMs;
    double fReleaseMs;
    double fMakeupDb;
    double fSampleRate = 48000.0;

    // Smoothing coefficients (per-sample) and cached linear gains.
    double fAttackCoef  = 0.0;
    double fReleaseCoef = 0.0;
    double fMakeupLin   = 1.0;

    // Stereo-linked smoothed GAIN envelope (linear, 1 = no reduction).
    double fEnv = 1.0;
};

} // namespace daw
