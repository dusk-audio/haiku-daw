// RangeEdit — the time-range edit arithmetic (M2.4), kit-free and host-tested.
//
// A time range is a span of frames with no clip identity of its own: the range
// operations have to decide, per clip, what part of it lies inside. That
// decision is the same for a range Copy (slice the content out), a range Delete
// (make it silent) and an Insert Silence (ripple everything from the range start
// right by the range length), so the arithmetic lives here once and the
// commands and the clipboard both call it.
//
// A sliced clip's startFrame is RELATIVE to the range start, which is what a
// paste needs (place it at playhead + startFrame) and what a pasted range keeps
// its internal spacing from.
#pragma once

#include "Project.h"
#include "types.h"

#include <algorithm>
#include <vector>

namespace daw {

// A clip (or MIDI region) reduced to a range, positioned relative to the range
// start. `audio` and `region` are alternatives; `midi` says which is live.
struct RangeItem {
    bool     midi  = false;
    TrackId  track = kInvalidTrackId;   // the track it came from
    Clip     audio;
    MidiClip region;
};

// Length of the intersection of [start, start+len) and [a, b): 0 = none.
inline Frame RangeOverlap(Frame start, Frame len, Frame a, Frame b) {
    const Frame lo = std::max(start, a);
    const Frame hi = std::min(start + len, b);
    return hi > lo ? hi - lo : 0;
}

// Trim a fade to a piece that lost `trimFront` frames at the front and
// `trimBack` at the back: the part of the ramp that survives is the original
// minus the trimmed amount, and never longer than what is left.
inline Frame TrimFade(Frame fade, Frame trimFront, Frame newLength) {
    if (fade <= 0) return 0;
    const Frame left = fade - trimFront;
    if (left <= 0) return 0;
    return std::min(left, newLength);
}

// Slice one audio clip to [a,b). Returns false when nothing of it lies inside;
// otherwise `out` is the piece (range-relative start, source offset advanced,
// fades trimmed to match) and `*startRel` is its start relative to `a`.
bool SliceClipToRange(const Clip& c, Frame a, Frame b, Clip* out);

// The MIDI-region twin: the region's window is trimmed, its notes and events
// are rebased so they keep their absolute positions, and content that starts
// outside the range is dropped — a range copy is what the range sounds like,
// not a window onto material the user cannot see or hear.
bool SliceRegionToRange(const MidiClip& c, Frame a, Frame b, MidiClip* out);

// Everything that SOUNDS inside [a,b), in track then clip order: audio clips
// and MIDI regions, both ends cut to the range. Inactive loop-record takes are
// skipped (you copy what you can see; a paste of the result is an ordinary
// clip, not a new take group), and muted clips are copied as they are — their
// mute comes with them. A reversed range (b < a) yields nothing.
std::vector<RangeItem> ExtractRange(const Project& p, Frame a, Frame b);

} // namespace daw
