// Reverb — a simple Schroeder/Freeverb-style reverberator, one of the built-in
// effects.
//
// Topology adapted from the user's DuskVerb DSP conventions (kit-free port):
// four parallel LOW-PASS-FEEDBACK comb filters (Freeverb "LBCF" — a damping
// one-pole inside each comb's feedback path, as DuskVerb damps its tank loop)
// summed and passed through two series canonical Schroeder allpass diffusers
// (the exact lattice form used by DuskVerb's DattorroTank::Allpass:
// vn = in + g*vd, out = vd - g*vn). roomSize drives the comb feedback
// (Freeverb's roomsize*0.28 + 0.7 map), mix sets wet/dry. Delay lengths are the
// classic prime tunings scaled from the 44.1 kHz calibration rate (the
// kBaseSampleRate convention DuskVerb uses). A tiny DC offset on the feedback
// path prevents denormal slowdown, mirroring DspUtils::kDenormalPrevention.
//
// All delay lines are sized in Prepare() from the sample rate; Process() is pure
// arithmetic on preallocated state. Kit-free (STL only), host-testable.
#pragma once

#include "IEffect.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace daw {

class Reverb : public IEffect {
public:
    static constexpr int kNumCombs   = 4;
    static constexpr int kNumAllpass = 2;

    // roomSize = comb feedback / tail length [0,1], mix = wet amount [0,1]
    // (0 = dry only, 1 = wet only).
    Reverb(double roomSize = 0.5, double mix = 0.3);

    void SetParams(double roomSize, double mix);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;
    const char* Name() const override { return "Reverb"; }

private:
    // A single delay line (used by both comb and allpass stages).
    struct Line {
        std::vector<float> buf;
        int    size = 1;
        size_t pos  = 0;
        void   Init(int n) { size = n < 1 ? 1 : n; buf.assign((size_t)size, 0.0f); pos = 0; }
        void   Clear() { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    };

    void BuildLines();

    double fRoomSize;
    double fMix;
    double fSampleRate = 48000.0;

    // Per-channel comb bank (each with its own damping one-pole state) and
    // series allpass bank.
    Line  fComb[2][kNumCombs];
    float fCombLP[2][kNumCombs] = {};   // Freeverb damping filter state
    Line  fAllpass[2][kNumAllpass];
};

} // namespace daw
