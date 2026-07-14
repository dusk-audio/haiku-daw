// Saturator — analog tape / FET saturation, one of the built-in effects.
//
// Ported from the user's tape-echo plugin DSP (plugins/tape-echo/core/
// TapeEchoDSP.hpp): the tape/FET saturation core is
//   * softClip(x)     — a bounded cubic, tanh-like waveshaper. Monotonic on its
//                       clamped range with |out| <= 1; this is the tape
//                       saturation curve that also stabilises the plugin's
//                       feedback loop.
//   * preampShape(x)  — the FET preamp front-end: a mild x^2 asymmetry (even
//                       harmonics — the "bias" character) fed through softClip.
//                       The asymmetry injects DC, so a per-channel DC blocker
//                       (DuskFilters::DCBlocker) removes it downstream.
//   * drive taper     — 0.4 + 2.6*d^2 (donor audio taper: clean at the bottom,
//                       saturated at the top) with 1/softClip(drive) peak
//                       compensation so loudness stays roughly constant as drive
//                       rises.
// The framework (JUCE/DPF) types are stripped; only the arithmetic is kept.
//
// Real-time contract: coefficients + DC-blocker state are set up in Prepare();
// Process() is pure arithmetic on preallocated state. Kit-free (STL only),
// host-testable.
#pragma once

#include "IEffect.h"

namespace daw {

class Saturator : public IEffect {
public:
    // drive       = saturation amount [0,1] (0 = nearly clean, 1 = hard tape sat)
    // mix         = dry/wet [0,1] (0 = dry only, 1 = fully saturated)
    // outputTrimDb = output trim applied to the wet signal, in dB
    Saturator(double drive = 0.5, double mix = 1.0, double outputTrimDb = 0.0);

    void SetParams(double drive, double mix, double outputTrimDb);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;
    const char* Name() const override { return "Saturator"; }

private:
    void Recompute();

    double fDrive;          // [0,1] knob
    double fMix;            // [0,1] dry/wet
    double fOutputTrimDb;   // wet output trim, dB
    double fSampleRate = 48000.0;

    // Cached, derived from the params (donor taper + peak compensation).
    float fDriveMult = 1.0f;   // 0.4 + 2.6*drive^2
    float fDriveComp = 1.0f;   // 1 / softClip(driveMult) — peak/loudness comp
    float fTrimLin   = 1.0f;   // dB -> linear

    // Per-channel DC blocker state (removes the even-harmonic asymmetry's DC).
    float fDcR   = 0.9975f;    // pole; tracks sample rate (~20 Hz corner)
    float fDcX1L = 0.0f, fDcY1L = 0.0f;
    float fDcX1R = 0.0f, fDcY1R = 0.0f;
};

} // namespace daw
