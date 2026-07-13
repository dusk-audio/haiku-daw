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
    // amount [0,1] (0 = dry only, 1 = wet only).
    Delay(double delaySeconds = 0.25, double feedback = 0.3, double mix = 0.3);

    void SetParams(double delaySeconds, double feedback, double mix);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    const char* Name() const override { return "Delay"; }

private:
    double fDelaySec;
    double fFeedback;
    double fMix;
    double fSampleRate = 48000.0;

    std::vector<float> fBuf;      // interleaved stereo delay line
    int    fDelayFrames = 1;
    size_t fPos = 0;
};

} // namespace daw
