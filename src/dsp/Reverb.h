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
#include <memory>
#include <vector>

namespace daw {

// Reverb algorithm selector (param slot 2). 0 keeps the original Freeverb-style
// reverb (back-compatible with old projects, whose params stop at [size, mix]);
// higher values select a ported DuskVerb engine.
enum class ReverbAlgo { Freeverb = 0, DuskPlate = 1, DuskHall = 2, DuskFDN = 3 };

class Reverb : public IEffect {
public:
    static constexpr int kNumCombs   = 4;
    static constexpr int kNumAllpass = 2;

    // Param layout: [size, mix, algorithm, decaySec, tone]. size = comb
    // feedback / tail length [0,1]; mix = wet amount [0,1]; algorithm per
    // ReverbAlgo; decaySec = reverb time (engine algos); tone [0,1] brightness.
    Reverb(double roomSize = 0.5, double mix = 0.3);
    ~Reverb() override;   // out-of-line for the forward-declared plate

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

    void BuildEngine();  // (re)create + configure the selected DuskVerb engine
    void ApplyEngineParams();

    double fRoomSize;
    double fMix;
    double fSampleRate = 48000.0;
    int    fAlgo  = 0;      // ReverbAlgo
    double fDecay = 2.0;    // reverb time (s) for engine algorithms
    double fTone  = 0.5;    // brightness [0,1]

    // Per-channel comb bank (each with its own damping one-pole state) and
    // series allpass bank (Freeverb algorithm).
    Line  fComb[2][kNumCombs];
    float fCombLP[2][kNumCombs] = {};   // Freeverb damping filter state
    Line  fAllpass[2][kNumAllpass];

    // DuskVerb engines (owned behind a pImpl so the heavy/templated engine
    // headers stay in the .cpp) + de-interleave scratch (processed in chunks).
    struct Engines;
    std::unique_ptr<Engines> fEng;
    bool HasEngine() const;   // true if a DuskVerb engine is selected + built
    static constexpr int kPlateChunk = 1024;
    std::vector<float> fInL, fInR, fOutL, fOutR;
};

} // namespace daw
