// Delay — a simple stereo feedback delay line, one of the built-in effects.
//
// Per-channel delay buffer sized in Prepare(); Process() reads the delayed
// sample, mixes it with the dry signal, and writes input + feedback back into
// the line. Kit-free, host-testable.
#pragma once

#include "IEffect.h"

#include <cstddef>
#include <vector>

namespace daw {

class Delay : public IEffect {
public:
    // delaySeconds = echo time, feedback = decay per repeat [0,1), mix = wet
    // amount [0,1] (0 = dry only, 1 = wet only). sync locks the time to a note
    // division of the tempo; division indexes kDivisions (see Delay.cpp).
    Delay(double delaySeconds = 0.25, double feedback = 0.3, double mix = 0.3,
          bool sync = false, int division = 0);

    void SetParams(double delaySeconds, double feedback, double mix);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;
    void SetTempo(double bpm) override;
    const char* Name() const override { return "Delay"; }

    static constexpr int kDivisionCount = 7;
    // Note-division label for the editor (1/4, 1/4., 1/4T, 1/8, ...).
    static const char* DivisionName(int i);

private:
    // Recompute the read-tap distance (fDelayFrames) from the current time /
    // sync / division / tempo. RT-safe: integer math on a preallocated buffer.
    void UpdateTap();

    double fDelaySec;
    double fFeedback;
    double fMix;
    bool   fSync;
    int    fDivision;
    double fBpm        = 120.0;
    double fSampleRate = 48000.0;

    std::vector<float> fBuf;      // interleaved stereo delay line (max-sized)
    int    fCapFrames   = 1;      // buffer capacity in frames (max delay)
    int    fDelayFrames = 1;      // current read-tap distance (<= fCapFrames)
    size_t fPos = 0;              // write cursor
};

} // namespace daw
