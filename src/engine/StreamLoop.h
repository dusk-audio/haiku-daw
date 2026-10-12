// StreamLoop — the arithmetic of looping playback (M4.2).
//
// A loop is a discontinuity in the transport: at the loop end the position jumps
// to the loop start. M4.2 makes that jump happen INSIDE the running graph, with
// no rebuild and no device re-create, which needs two threads to agree on where
// the seam is without ever talking to each other:
//
//   - the RT callback (`Engine::Graph::Render`) splits the callback's block at
//     the wrap frame, so the jump is sample-exact rather than block-granular;
//   - each clip stream's disk thread produces the clip's audio in PLAYBACK
//     ORDER across the seam — the pass it plays in one loop pass, then the pass
//     it plays in the next — so its ring is a FIFO whose next frames are always
//     the frames the transport is about to ask for. The loop start is pre-rolled
//     by the same mechanism that keeps the ring ahead of the playhead: the disk
//     thread is already there, and it never needs to seek at the seam itself.
//
// Both sides derive their behaviour from the same range and the same rule, which
// is why that arithmetic lives here — kit-free and host-tested — instead of
// inside the two callers. A stream's window is [clipStart, clipEnd); a pass is
// the part of it one loop pass visits; `wraps` is how many times the transport
// has crossed the loop end, which is what makes "where on the looped path are
// we" unambiguous (a position inside a loop is reachable after any number of
// passes).
#pragma once

#include "../model/types.h"
#include "StreamRebase.h"

#include <cstddef>
#include <cstdint>

namespace daw {

// The loop a clip stream plays under, as the graph resolved it (LoopActive
// already applied). Passed to the streams at build, so the disk thread and the
// RT callback derive their seam from one description.
struct StreamLoopRange {
    bool  on    = false;
    Frame start = 0;
    Frame end   = 0;
};

// The shortest loop the engine will walk. A loop shorter than the block the
// callback renders in would have to split one callback into an unbounded number
// of segments, each of which renders the whole node graph. A range below this is
// treated as "not looping" — by the RT wrap AND by the disk threads' pass
// walker, so the rule lives in one place: they must agree, or a stream would
// produce passes the transport never visits.
inline bool LoopActive(bool enabled, Frame loopStart, Frame loopEnd,
                       Frame blockFrames) {
    if (!enabled || loopEnd <= loopStart) return false;
    const Frame minLen = blockFrames > 0 ? blockFrames : 1;
    return loopEnd - loopStart >= minLen;
}

// Frames of [from, to) that fall inside the clip's window: what a stream
// consumes of a span, and so what its disk thread must produce for one.
inline Frame WindowSpanFrames(Frame clipStart, Frame clipEnd, Frame from, Frame to) {
    if (clipEnd <= clipStart) return 0;
    if (from < clipStart) from = clipStart;
    if (to > clipEnd) to = clipEnd;
    return to > from ? to - from : 0;
}

// How much of a callback block the RT renders before it must wrap. 0 means the
// block starts at or after the loop end and wraps before rendering anything.
// Only meaningful while the loop is active (LoopActive).
inline size_t SegmentFrames(Frame pos, size_t frames, Frame loopStart, Frame loopEnd) {
    if (pos >= loopEnd) return 0;
    const Frame until = loopEnd - pos;
    return until < (Frame)frames ? (size_t)until : frames;
}

// The source frame a clip's own timeline frame maps to: what the disk thread
// seeks to at the start of a pass, and what `Prepare` seeks to for the first
// one. One rounding rule for both, so a pass start and the graph's opening seek
// cannot land a frame apart.
inline int64_t PassSourceFrame(int64_t sourceOffset, Frame clipStart, Frame t,
                               double srcRate, double outRate) {
    const double d = (double)(t - clipStart)
                     * (outRate > 0.0 ? srcRate / outRate : 1.0);
    return sourceOffset + (int64_t)(d + 0.5);
}

// The pass walker: the timeline ranges one clip stream's disk thread produces,
// in playback order.
//
//   Start(origin) — the first pass, from where this graph's playback begins
//                   (the clip's own seek position) to the first cut (the loop
//                   end, or the clip's end when there is no loop);
//   Next()        — the loop's own pass, [loopStart, loopEnd) ∩ the window,
//                   repeated once per pass for as long as the stream runs.
//
// A pass is empty when the transport's visit does not meet the clip's window
// there (a clip entirely behind the loop start, or one that begins after the
// loop end); the walker skips to the loop pass rather than producing silence for
// a range that never plays, and reports `done` when no pass is left at all.
struct ProduceCursor {
    Frame clipStart = 0;
    Frame clipEnd   = 0;
    bool  looping   = false;   // LoopActive(), not merely "loop enabled"
    Frame loopStart = 0;
    Frame loopEnd   = 0;

    Frame origin = 0;          // the first pass's own start (clip start + seek)
    int   pass   = 0;          // 0 = the first pass, 1 = a loop pass (repeats)
    bool  done   = false;
    Frame lo = 0, hi = 0;      // the current pass: timeline frames [lo, hi)

    void Start(Frame firstPassLo) {
        origin = firstPassLo;
        pass   = 0;
        done   = false;
        Recompute();
    }

    // Advance to the next pass. False (and `done`) when there is none: the clip
    // is behind the loop, begins after the loop ends, or the transport does not
    // loop at all.
    bool Next() {
        if (done) return false;
        if (pass == 0) pass = 1;
        Recompute();
        return !done;
    }

    Frame Frames() const { return hi > lo ? hi - lo : 0; }

private:
    void Recompute() {
        for (;;) {
            const Frame passLo = (pass == 0) ? origin : loopStart;
            const Frame passHi = (pass == 0) ? (looping ? loopEnd : clipEnd)
                                             : loopEnd;
            lo = passLo > clipStart ? passLo : clipStart;
            hi = passHi < clipEnd ? passHi : clipEnd;
            if (hi > lo) { done = false; return; }
            // Nothing to produce in this pass. While looping the transport has
            // not visited it at all (a first pass that starts past the cut), so
            // walk on to the loop's own pass; otherwise the clip is spent.
            if (pass == 0 && looping) { pass = 1; continue; }
            done = true;
            lo = hi = 0;
            return;
        }
    }
};

// How many ring frames the RT must drop to be aligned with `target`, walking the
// LOOPED path the transport took from `origin`: the first pass's tail, the
// complete loop passes, and the current pass's head. `wraps` is the number of
// times the transport crossed the loop end since the graph was requested (the
// engine's own counter), which is what makes the count unambiguous.
//
// Only the IN-PLACE join needs this. A position-changing load joins at its own
// origin by construction — its playhead is set there and nothing advances while
// no graph is active — so its `wraps` is 0, and a wrap count that includes
// blocks rendered by the graph it replaced must not reach it (Engine::Graph
// keeps the two apart).
//
// It also assumes the graph and the one it replaces walk the SAME loop, which
// is why a loop-range change is a position change and not an in-place edit: the
// pass structure the count indexes would otherwise differ between the two.
//
// Without a loop (or with no wrap since the request) this is exactly
// RebaseSkipFrames — the M4.1 in-place join — and the two must agree there.
inline Frame LoopRebaseSkipFrames(Frame origin, Frame clipStart, Frame clipEnd,
                                  Frame target, bool looping, Frame loopStart,
                                  Frame loopEnd, uint64_t wraps) {
    if (!looping || wraps == 0 || loopEnd <= loopStart)
        return RebaseSkipFrames(origin, clipStart, clipEnd, target);
    Frame skip = WindowSpanFrames(clipStart, clipEnd, origin, loopEnd);
    const Frame perPass = WindowSpanFrames(clipStart, clipEnd, loopStart, loopEnd);
    // wraps == 1: the transport is in its first loop pass (the second pass it
    // plays); each further wrap completes one more.
    if (wraps > 1) skip += (Frame)(wraps - 1) * perPass;
    skip += WindowSpanFrames(clipStart, clipEnd, loopStart, target);
    return skip;
}

} // namespace daw
