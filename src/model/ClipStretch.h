// ClipStretch — the model-side policy of time-stretch (M7.2), kit-free.
//
// A clip may play slower or faster than its source without changing pitch. The
// model stores that as `Clip::stretchRatio`, an OUTPUT DURATION MULTIPLIER:
// 1.0 = off, 2.0 = twice as long (half speed), 0.5 = twice as fast. The pinned
// convention (see docs/agent-prompts/06-timestretch.md) is:
//
//     lengthFrames is the TIMELINE length (unchanged by stretching);
//     source frames consumed = lengthFrames / stretchRatio
//     (in the source file's own frames: lengthFrames / ratio * srcRate/projRate)
//
// Fades, fades, crossfades, automation and the transport all stay in the
// timeline domain, which is why the ratio lives only where source material is
// read: the engine's disk path, the exporter, and the waveform drawing.
//
// The two policy decisions here are the clamp and the drag derivation; both are
// host-tested (tests/stretch_tests.cpp) because getting them wrong is silent —
// a clip that reads past its trimmed span still sounds like audio.
#pragma once

#include "Project.h"

#include <cmath>

namespace daw {

// The ratio range the model accepts. Below 0.25 (4x) WSOLA smears transients
// audibly; above 4.0 (4x slower) the same. Both the command and the loader
// clamp into this range, so a hand-edited or damaged file cannot put the
// engine into a state the DSP was never designed for.
constexpr double kMinStretchRatio = 0.25;
constexpr double kMaxStretchRatio = 4.0;

// Clamp a ratio into the accepted range. Non-positive and NaN values (a
// hand-edited file, a division that went wrong) mean "no stretch" — 1.0 — not
// "the minimum", because a garbage value must never silently turn into a 4x
// stretch.
inline double ClampStretchRatio(double r) {
    if (!(r > 0.0)) return 1.0;                 // NaN, 0, negative
    if (r < kMinStretchRatio) return kMinStretchRatio;
    if (r > kMaxStretchRatio) return kMaxStretchRatio;
    return r;
}

// A clip's effective ratio: always in range, even for a model built without the
// command (a loaded file, a test fixture).
inline double ClipStretchRatio(const Clip& c) {
    return ClampStretchRatio(c.stretchRatio);
}

// True for a clip that actually needs the stretch stage. The engine, the
// exporter and the drawing all key their stretch path off this, so a ratio of
// 1.0 keeps exactly the code path it had before the feature existed.
inline bool ClipIsStretched(const Clip& c) {
    return ClipStretchRatio(c) != 1.0;
}

// The clip's referenced source span, in SOURCE FILE frames: what playback reads
// from `sourceOffset` onward. (Timeline length divided by the ratio, then
// converted to the file's own rate.)
inline double ClipSourceSpanFrames(const Clip& c, double srcRate, double projRate) {
    const double rate = (srcRate > 0.0 && projRate > 0.0) ? srcRate / projRate : 1.0;
    return (double)c.lengthFrames * rate / ClipStretchRatio(c);
}

// The ratio a stretch-to-fit drag to `newTimelineLen` produces.
//
// It is derived from the clip's CURRENT SOURCE SPAN — the material the clip
// references today, lengthFrames / oldRatio — and never from the source file's
// whole length: a clip is a non-destructive window into its file (a loop
// dragged in from the browser is always trimmed), so the file length would both
// compute a wildly wrong ratio and drag material the user trimmed away into the
// clip.
//
// `fileFrames` is the source file's frame count (0 = unknown, no clamp). When
// it is known, the span is clamped to what the file actually holds from
// `sourceOffset`: a clip whose span already runs off the end of its file (a
// truncated take, a hand-edited project) still derives a finite ratio from the
// material that exists instead of asking for material that never did.
inline double StretchRatioForDrag(const Clip& c, Frame newTimelineLen,
                                  Frame fileFrames, double srcRate,
                                  double projRate) {
    const double rate = (srcRate > 0.0 && projRate > 0.0) ? srcRate / projRate : 1.0;
    double spanSrc = ClipSourceSpanFrames(c, srcRate, projRate);
    if (fileFrames > 0) {
        const double avail = (double)(fileFrames - c.sourceOffset);
        if (avail < spanSrc) spanSrc = avail;   // clamp to the file's real end
    }
    if (spanSrc < 1.0) spanSrc = 1.0;           // degenerate: keep it finite
    const double spanTimeline = spanSrc / rate; // that span, in timeline frames
    return ClampStretchRatio((double)newTimelineLen / spanTimeline);
}

} // namespace daw
