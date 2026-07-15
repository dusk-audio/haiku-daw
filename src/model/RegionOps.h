// RegionOps — pure, kit-free DSP over decoded audio buffers, shared by the
// clip "region operations" (normalize / reverse / strip-silence). No Media Kit,
// no file I/O: callers decode a clip to an interleaved-stereo float buffer and
// hand it here, so every function is host-unit-testable on a synthetic buffer.
//
// Buffers are always 2-channel interleaved float (the format WavSource emits):
// frame f is buf[2*f] (L) and buf[2*f+1] (R). Frame counts are in these frames.
#pragma once

#include <cstdint>
#include <vector>

namespace daw {

// Largest absolute sample across both channels (0 for an empty/silent buffer).
// A normalize op divides into this to reach unity peak.
float PeakLinear(const float* interleaved, int64_t frames);

// Reverse the buffer in place, frame by frame (both channels move together).
void ReverseStereo(float* interleaved, int64_t frames);

// A half-open [start, end) frame range to keep.
struct Span { int64_t start = 0; int64_t end = 0; };

// Split a buffer into the spans worth keeping, dropping runs of near-silence.
// A frame is "silent" when max(|L|,|R|) < threshLinear. Only silence runs at
// least `minSilence` frames long are treated as gaps; each gap is shrunk by
// `pad` frames on both sides so the kept spans retain a little air (no clicks).
// Leading/trailing silence is trimmed the same way. Returns kept spans in
// order; a buffer with no qualifying gaps yields a single [0, frames) span.
std::vector<Span> NonSilentSpans(const float* interleaved, int64_t frames,
                                 float threshLinear, int64_t minSilence,
                                 int64_t pad);

} // namespace daw
