// Resampler — streaming linear sample-rate conversion for stereo float.
//
// The engine mixes at one output rate, but sources (and recorded takes) can be
// any rate. Rather than complicate the real-time mixer, each stream resamples
// on its disk thread so the ring the RT callback drains is already at the
// output rate (1 ring frame == 1 output/timeline frame). This class does that
// conversion incrementally: feed it source-rate chunks, it appends output-rate
// frames, carrying interpolation state across calls.
//
// Linear interpolation (v1): cheap, kit-free, host-testable. A windowed-sinc
// upgrade can drop in behind the same interface. Fixed 2-channel interleaved.
#pragma once

#include <cstddef>
#include <vector>

namespace daw {

class Resampler {
public:
    // inRate = source frame rate, outRate = engine output rate. A ratio of 1
    // (equal rates) passes samples through unchanged.
    Resampler(double inRate, double outRate);

    // Convert `inFrames` interleaved-stereo source frames, appending the
    // resulting output-rate interleaved-stereo frames to `out`.
    void Process(const float* in, size_t inFrames, std::vector<float>& out);

    // Ratio of output frames produced per input frame (outRate / inRate).
    double Ratio() const { return fRatio; }
    bool   IsIdentity() const { return fStep == 1.0; }

private:
    double fRatio;          // outRate / inRate
    double fStep;           // input frames advanced per output frame (inRate/outRate)
    double fPos;            // fractional position of next output, in [0,1)
    float  fPrevL, fPrevR;  // previous input frame (interpolation left edge)
    bool   fHavePrev;
};

} // namespace daw
