// ArrangeLogic — the arrangement view's kit-free rules.
//
// The arrange gestures live in `src/ui/TimelineView.cpp`, which only compiles
// on Haiku. The rules behind four of them are pure functions of the model —
// which block a point is on, what the loop adopts when it has no range, how
// far a scrollbar step moves the view, and which selection Delete acts on —
// so they live here, where `ctest --test-dir build-host` can pin them
// (`tests/arrange_logic_tests.cpp`), and the view calls exactly these.
//
// Same layering as `SnapGrid.h` / `RangeEdit.h` / `Fade.h`: STL only.
#pragma once

#include "SnapGrid.h"    // SnapBarBeats — the bar maths the loop rule needs
#include "TempoMap.h"
#include "types.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace daw {

// --- which block a point is on (the draw-order rule) ------------------------
//
// Clips and MIDI regions are DRAWN in `startFrame` order (DrawLanes walks the
// vector, and Project::AddClip keeps it sorted), so of two overlapping blocks
// the LATER one is painted on top — and that is the one the pointer must act
// on. A hit test that returned the first match acted on the block underneath
// the crossfade: the click, the hover highlight, the cursor and the scissors'
// cut line all followed the wrong clip.
//
// `at` is a frame (XToFrame(where.x)). `skip` passes over the items the caller
// does not draw — an inactive loop-record take. Ties (two blocks with the same
// start) go to the later index, which is also the one drawn last. Returns -1
// when no block contains the point.
template <class Item, class Skip>
inline int TopmostClipIndexAt(const std::vector<Item>& items, Frame at, Skip skip) {
    int best = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (skip(items[i])) continue;
        const Frame s = items[i].startFrame;
        if (at < s || at >= s + items[i].lengthFrames) continue;
        // A greater start wins; an equal start goes to the later index.
        if (best < 0 || s >= items[(std::size_t)best].startFrame) best = (int)i;
    }
    return best;
}

// --- the loop range to adopt when none is set -------------------------------
//
// Turning looping on with no range has to END UP with a range the engine can
// walk: with a zero-length one the lamp (TransportBar) and the status line
// claim a loop that the engine ignores. The default is the BAR around the
// playhead — the block the user is working in — the same way a DAW sets its
// locators to the current bar rather than refusing.
//
// Returns false when no bar can be built (nothing to adopt); the caller must
// then refuse the toggle and say so instead of lighting the lamp.
inline bool DefaultLoopRange(const TempoMap& tm, Frame playhead, Frame* lo,
                             Frame* hi) {
    if (!lo || !hi) return false;
    if (playhead < 0) playhead = 0;
    const double barBeats = SnapBarBeats(tm, playhead);
    if (!(barBeats > 0.0)) return false;

    // The beat the governing meter change starts at, then the bar containing
    // the playhead — the same arithmetic SnapFrame's `Bar` case uses, so a
    // drum of the ruler grid and the adopted loop cannot disagree.
    double segBeat = 0.0;
    for (const MeterChange& m : tm.Meters()) {
        if (m.frame > playhead) break;
        segBeat = tm.BeatAt(m.frame);
    }
    const double beat = tm.BeatAt(playhead);
    const double bars = std::floor((beat - segBeat) / barBeats);

    Frame a = tm.FrameAt(segBeat + bars * barBeats);
    const Frame b = tm.FrameAt(segBeat + (bars + 1.0) * barBeats);
    if (a < 0) a = 0;
    if (b <= a) return false;         // nothing to loop
    *lo = a;
    *hi = b;
    return true;
}

// --- the scrollbars' step sizes --------------------------------------------
//
// A BScrollBar's steps are in the scrollbar's VALUE units, and the timeline's
// horizontal range is FRAMES (SyncScrollBars: SetRange(0, ContentEndFrame())),
// not pixels. The constructor's stock SetSteps(12, 120) therefore moved the
// view by twelve frames — at the default 512 frames/px that is a fiftieth of a
// pixel: an arrow click visibly did nothing, and a trough click did nothing
// twice over.
//
// `small` is a fixed PIXEL distance (the arrow's job is to nudge by something
// you can see) and `large` is one screenful (the trough's). Both are floored at
// one frame so a step can never be a no-op.
inline void ScrollStepsFor(double framesPerPixel, float visibleWidthPx,
                           float* small, float* large) {
    if (framesPerPixel < 1e-9) framesPerPixel = 1e-9;
    if (visibleWidthPx < 1.0f) visibleWidthPx = 1.0f;
    const double s = 16.0 * framesPerPixel;               // ~16 px of frames
    const double l = (double)visibleWidthPx * framesPerPixel;   // one screen
    *small = (float)(s < 1.0 ? 1.0 : s);
    *large = (float)(l < 1.0 ? 1.0 : l);
}

// --- what Delete / Cut / Copy act on ---------------------------------------
//
// The arrangement has two selections — a time range and a set of clips — and
// the view keeps them MUTUALLY EXCLUSIVE: the gesture the user just made wins.
// A range that gets set drops the clip selection, and a clip-selection gesture
// (a click, a Shift-click, a band, Select All) drops the range, so no
// operation can act on a selection the user is no longer looking at. Without
// that invariant Edit > Delete took a stale range even after the user had just
// clicked a clip: what was deleted was not what the click selected.
enum class SelectTarget { None, Range, Clips };

inline SelectTarget SelectionTargetFor(bool rangeValid, bool clipSelection) {
    if (rangeValid) return SelectTarget::Range;
    if (clipSelection) return SelectTarget::Clips;
    return SelectTarget::None;
}

} // namespace daw
