// SincResampler — streaming windowed-sinc sample-rate conversion for stereo
// float, for the OFFLINE path (the exporter's per-clip decode).
//
// Resampler (src/engine/Resampler.h) is a linear interpolator: cheap, and good
// enough for a monitor signal nobody is mastering from — its images fold back
// at only -10..-20 dB. A bounce is the master, so the offline path uses a real
// band-limiting filter instead: a Kaiser-windowed sinc, evaluated through a
// polyphase table, so a downsample's aliases land ~90 dB down and the passband
// is flat to the edge of the band.
//
// Same interface shape as Resampler (streaming, interleaved, carried state),
// so a caller switches its local variable and nothing else:
//
//   SincResampler rs(src.FrameRate(), outRate);
//   while (read chunk) rs.Process(chunk, frames, out);
//
// Kit-free (STL only). Construction builds the polyphase table (allocation,
// ~800 kB, a few ms); Process only reads it and arithmetic on carried state,
// so the class is safe to use from any non-RT thread and allocates nothing
// after construction beyond the caller's `out` growing.
//
// Identity (inRate == outRate, exactly) is a bit-exact passthrough rather than
// a kernel run: the exporter's own renders are asserted sample-exact, and a
// clip already at the output rate must stay untouched — including a project
// exported at its own sample rate, whose file must not change because of the
// filter's rounding.
#pragma once

#include <cstddef>
#include <vector>

namespace daw {

class SincResampler {
public:
    static constexpr int kMaxChannels = 2;

    // Kernel shape. kTaps is the number of input samples each output sample is
    // built from, kPhases the polyphase table's resolution (the fractional
    // position is snapped to 1/kPhases of an input sample).
    //
    // kTaps = 192 and beta = 9.0 are a Kaiser design for ~90 dB of stopband
    // attenuation at a transition width of ~0.03 cycles/input-sample, which
    // leaves the passband flat past 21 kHz for a 96k source and puts a 30 kHz
    // tone 6 kHz inside the stopband on a 96k -> 48k downsample. Fewer taps
    // would widen the transition until the guard band no longer clears the
    // passband; more is CPU the offline path does not need.
    static constexpr int    kTaps       = 192;
    static constexpr int    kPhases     = 1024;
    static constexpr double kKaiserBeta = 9.0;

    // inRate = source frame rate, outRate = destination frame rate. `channels`
    // is the interleave of both input and appended output (1 or 2), clamped;
    // feeding a mono buffer with the stereo count would read past each frame.
    SincResampler(double inRate, double outRate, int channels = 2);

    // Convert `inFrames` interleaved source frames (fChannels each), appending
    // the resulting destination-rate interleaved frames to `out`.
    //
    // Only output frames whose whole filter window is in the data are emitted:
    // a window that reached into samples the caller has not handed over yet
    // would make the result depend on how the stream was CHUNKED, which the
    // decode loop (WavSource::ReadChunk decides the sizes) must not. The
    // consequence is that the last kTaps/2 input frames' worth of output
    // stays pending until either more input arrives or Flush() says there is
    // no more.
    void Process(const float* in, size_t inFrames, std::vector<float>& out);

    // The end of the stream: emit the output still pending, treating the
    // samples after the last one as silence. Call it once, after the last
    // Process(), or the last ~kTaps/2 input frames produce no output (a
    // resampled clip would lose a couple of milliseconds off its tail).
    // The trailing output frames fade out over the filter's own length,
    // because the signal after the end of a file is not knowable; a caller
    // that wants a clean tail asks for a few frames more than it needs, which
    // is what the exporter already does.
    void Flush(std::vector<float>& out);

    // Drop the carried history and phase, for a caller that has just seeked the
    // source somewhere else. The next output frame then corresponds to the
    // first input frame of the next Process() again, with no tail of whatever
    // was decoded before.
    void Reset();

    // Destination frames produced per input frame (outRate / inRate).
    double Ratio() const { return fRatio; }
    bool   IsIdentity() const { return fIdentity; }
    int    Channels() const { return fChannels; }

private:
    // Build the polyphase table for this ratio: one normalized row per phase.
    void BuildKernel();

    // Append one output frame at the current position (both channels).
    void Emit(std::vector<float>& out);

    double fRatio   = 1.0;   // outRate / inRate
    double fStep    = 1.0;   // input frames advanced per output frame
    bool   fIdentity = false;
    int    fChannels = 2;

    // kPhases rows of kTaps weights, each normalised so a constant input maps
    // to exactly that constant.
    std::vector<float> fTable;

    // Carried input state. `fBuf` holds the frames still needed: the filter's
    // history at the front followed by whatever Process appended, with kTaps
    // zero frames of slack at the end so a window that reads past the data (the
    // stream's own end) reads silence instead of running off the buffer.
    std::vector<float> fBuf;      // interleaved
    size_t fValid = 0;            // real frames in fBuf (excludes the slack)
    double fPos   = 0.0;          // next output position, in frames into fBuf

    // The leading kTaps/2 frames of fBuf stand in for the source before its
    // first sample, so the first output frame's window never reads a negative
    // index. They hold the stream's FIRST sample (not zero): a clip trimmed at
    // a transient then starts at its own amplitude instead of swelling in over
    // a couple of milliseconds, and a clip that starts in silence is unaffected
    // either way. fPos starts at that same offset.
    static constexpr int kPrime = kTaps / 2;
    bool fNeedPrime = true;   // the primed frames still hold placeholders
};

} // namespace daw
