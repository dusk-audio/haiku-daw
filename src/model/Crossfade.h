// Crossfade — derive effective clip fades from timeline overlaps.
//
// When two clips on a track overlap in time, we auto-crossfade: the earlier
// clip fades out and the later fades in across the overlap. Rather than mutate
// the model, the renderer asks this helper for each clip's *effective* fade
// lengths — the clip's own fades combined (via max) with any auto-crossfade
// implied by overlapping its neighbor.
//
// Each effective fade also carries the SHAPE it must render with (M2.5): the
// clip's own fadeShape when the clip's own fade is the effective one, and
// EqualPower when the effective fade comes from an auto-crossfade — equal power
// is the DAW default for a crossfade, and the two mirrored ramps then sum to
// constant power. The drawing and both renderers read these fields, so the
// curve the user sees is the curve that sounds.
//
// Assumes `clips` is sorted ascending by startFrame (Project keeps it so).
// Kit-free (STL only), header-only, host-testable.
#pragma once

#include "Project.h"
#include "types.h"

#include <algorithm>
#include <vector>

namespace daw {

struct ClipFades {
    Frame     fadeIn  = 0;
    Frame     fadeOut = 0;
    // What each effective fade renders with. Set by ComputeCrossfades.
    FadeShape inShape  = FadeShape::Linear;
    FadeShape outShape = FadeShape::Linear;
};

// Frames by which clip `b` overlaps the clip `a` that immediately precedes it,
// or 0 when they do not overlap. This IS the auto-crossfade length: `a` fades
// out and `b` fades in across exactly this span, which begins at b.startFrame.
// The timeline draws the crossfade from this same helper, so what you see and
// what the engine/exporter render can never drift apart.
inline Frame CrossfadeOverlap(const Clip& a, const Clip& b) {
    // Loop-record takes are stacked alternatives, not crossfade partners, and
    // a MUTED clip is not a partner either: there is no audible material on
    // that side to fade from or into (M2.5).
    if (a.takeGroup > 0 || b.takeGroup > 0) return 0;
    if (a.muted || b.muted) return 0;
    const Frame aEnd = a.startFrame + a.lengthFrames;
    if (b.startFrame >= aEnd) return 0;                // no overlap

    Frame overlap = aEnd - b.startFrame;               // >0
    overlap = std::min(overlap, a.lengthFrames);
    overlap = std::min(overlap, b.lengthFrames);
    return overlap > 0 ? overlap : 0;
}

// The gain multiplier a renderer applies `rel` frames into a clip, from the
// effective fades and the shapes they render with. The engine's RT mix, the
// exporter and the timeline's waveform all call THIS, so the curve that sounds
// and the curve that is drawn cannot drift (the two ramps combine as a min,
// which is what a fade-in meeting a fade-out has always done here).
inline float ClipFadeEnvelope(Frame rel, Frame length, Frame fadeIn,
                              FadeShape inShape, Frame fadeOut,
                              FadeShape outShape) {
    float f = 1.0f;
    if (fadeIn > 0 && rel < fadeIn)
        f = FadeGainAt(inShape, (float)rel / (float)fadeIn);
    if (fadeOut > 0 && rel >= length - fadeOut) {
        const float fo = FadeGainAt(outShape,
                                    (float)(length - rel) / (float)fadeOut);
        if (fo < f) f = fo;
    }
    return f;
}

inline std::vector<ClipFades> ComputeCrossfades(const std::vector<Clip>& clips) {
    std::vector<ClipFades> out(clips.size());
    for (std::size_t i = 0; i < clips.size(); i++) {
        out[i].fadeIn      = clips[i].fadeInFrames;
        out[i].fadeOut     = clips[i].fadeOutFrames;
        out[i].inShape     = clips[i].fadeShape;
        out[i].outShape    = clips[i].fadeShape;
    }

    for (std::size_t i = 0; i + 1 < clips.size(); i++) {
        const Frame overlap = CrossfadeOverlap(clips[i], clips[i + 1]);
        if (overlap <= 0) continue;
        // The auto fade wins the length whenever it is longer than the clip's
        // own; the shape follows the fade that is actually rendered.
        if (overlap > out[i].fadeOut) {
            out[i].fadeOut  = overlap;
            out[i].outShape = FadeShape::EqualPower;
        }
        if (overlap > out[i + 1].fadeIn) {
            out[i + 1].fadeIn  = overlap;
            out[i + 1].inShape = FadeShape::EqualPower;
        }
    }
    return out;
}

} // namespace daw
