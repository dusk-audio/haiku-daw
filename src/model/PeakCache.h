// PeakCache — a precomputed min/max waveform envelope.
//
// A DAW must never scan a whole audio file to repaint a waveform. Instead we
// build this cache once (on import) by scanning the source top to bottom and
// recording, per fixed-size bucket of frames, the minimum and maximum sample
// amplitude in that bucket. Painting a waveform then reads a handful of
// buckets per pixel column — O(pixels), not O(samples).
//
// Kit-free (STL only): builds and unit-tests on any host. The UI layer maps
// pixel columns to frame ranges and calls Range() to get the min/max to draw.
//
// The envelope is mono: each bucket summarises the average of the source's
// stereo channels, which is what a single waveform lane shows. Amplitudes are
// in [-1, 1] float, matching IAudioSource output.
#pragma once

#include "types.h"

#include <cstddef>
#include <vector>

namespace daw {

class IAudioSource;

// One bucket's vertical extent. An empty/never-written bucket reads back as
// {0, 0} (a flat line), which is the correct thing to draw for silence.
struct Peak {
    float min = 0.0f;
    float max = 0.0f;
};

class PeakCache {
public:
    PeakCache() = default;

    // Scan `src` (from wherever its read cursor is — normally the start) to
    // end of data, filling the envelope at the given bucket size. Returns
    // false if src is invalid or framesPerBucket < 1. Consumes the source's
    // forward read cursor, so build once, before streaming for playback.
    bool Build(IAudioSource& src, int framesPerBucket = 256);

    bool   IsValid() const { return fFramesPerBucket > 0; }
    int    FramesPerBucket() const { return fFramesPerBucket; }
    size_t BucketCount() const { return fPeaks.size(); }
    Frame  TotalFrames() const { return fTotalFrames; }
    // Source sample rate the envelope was built from (0 if unknown). The UI
    // needs it to map timeline frames (output rate) to source frames.
    float  SampleRate() const { return fSampleRate; }

    // The bucket at index i (clamped-empty {0,0} if out of range).
    Peak At(size_t i) const {
        return i < fPeaks.size() ? fPeaks[i] : Peak{};
    }

    // Aggregate min/max over the frame span [startFrame, endFrame). This is
    // the paint-time entry point: pass the frame range that a single pixel
    // column covers and get the extremes to draw. Spans covering many buckets
    // (zoomed out) combine them; a sub-bucket span returns its bucket. Out-of
    // range or empty spans return {0, 0}.
    Peak Range(Frame startFrame, Frame endFrame) const;

private:
    int                fFramesPerBucket = 0;
    Frame              fTotalFrames     = 0;
    float              fSampleRate      = 0.0f;
    std::vector<Peak>  fPeaks;
};

} // namespace daw
