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
// upgrade can drop in behind the same interface. Interleaved, 1 or 2 channels
// (channel count fixed at construction; interleave of input == interleave out).
#pragma once

#include <cstddef>
#include <vector>

namespace daw {

class Resampler {
public:
    static constexpr int kMaxChannels = 2;

    // inRate = source frame rate, outRate = engine output rate. A ratio of 1
    // (equal rates) passes samples through unchanged. `channels` is the
    // interleave of both the input and the appended output (1 or 2); feeding a
    // mono buffer with the default stereo count would read past each frame.
    Resampler(double inRate, double outRate, int channels = 2);

    // Convert `inFrames` interleaved source frames (fChannels each), appending
    // the resulting output-rate interleaved frames to `out`.
    void Process(const float* in, size_t inFrames, std::vector<float>& out);

    // Ratio of output frames produced per input frame (outRate / inRate).
    double Ratio() const { return fRatio; }
    bool   IsIdentity() const { return fStep == 1.0; }
    int    Channels() const { return fChannels; }

private:
    double fRatio;          // outRate / inRate
    double fStep;           // input frames advanced per output frame (inRate/outRate)
    double fPos;            // fractional position of next output, in [0,1)
    int    fChannels;       // 1 or 2, interleave of in/out
    float  fPrev[kMaxChannels];  // previous input frame (interpolation left edge)
    bool   fHavePrev;
};

} // namespace daw
