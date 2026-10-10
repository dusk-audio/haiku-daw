// StreamRebase — re-aligning a clip stream when its graph is swapped in.
//
// A TrackStream's ring is primed with the clip's audio starting at the frame
// its graph was BUILT for ("the origin" — max(buildStart, clipStart); see
// TrackStream::Prepare). Inside the clip's window [clipStart, clipEnd) Mix
// consumes exactly one ring frame per timeline frame.
//
// A position-changing rebuild detaches the old graph first, so the transport
// stands still until the new graph is in and the origin IS the frame the
// callback is about to render. But a graph rebuilt IN PLACE — a structural edit
// during playback — is published while the transport keeps rolling on the old
// graph, so by then the callback's block starts at `target`, later than the
// origin. The stream must drop the frames it would have consumed in
// [origin, target) ∩ [clipStart, clipEnd), or it would play that far behind the
// transport for the rest of the clip's life.
//
// Kit-free and header-only so the arithmetic — the part a bug would live in —
// is host-tested (tests/engine_graph_tests.cpp), with the drop itself done by
// RingBuffer::Skip on the RT thread.
#pragma once

#include "../model/types.h"

namespace daw {

// How many stereo frames this stream must skip to be aligned with `target`.
// 0 when the target is at or before the origin, or the clip's window is empty
// or already behind the origin — all of which mean "nothing was consumed".
inline Frame RebaseSkipFrames(Frame origin, Frame clipStart, Frame clipEnd,
                              Frame target) {
    if (clipEnd <= clipStart) return 0;             // empty window: never reads
    Frame from = origin < clipStart ? clipStart : origin;
    if (from > clipEnd) from = clipEnd;             // origin past the clip
    Frame to = target < clipStart ? clipStart : target;
    if (to > clipEnd) to = clipEnd;                 // the clip ended meanwhile
    return to > from ? to - from : 0;
}

} // namespace daw
