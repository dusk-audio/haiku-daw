// StreamOrder — the order a build starts its clip streams filling in.
//
// A stream's fill IS the slow part of a graph build: the file read, the
// resample and the memcpy into a 2 s ring. MEASURED 2026-10-12 on the 2-vCPU
// VM with 329 clips: starting every stream's disk thread at once costs 680 ms
// of the build in pure contention (the builder thread starved by 329 fillers),
// while the file opens and thread starts themselves are 1 ms and 14 ms. So the
// build starts fills in bounded waves instead — and then the order matters:
// the wave that runs first is the one that is ready when the graph is
// published, so it has to hold the clips the transport needs first.
//
// Kit-free and host-tested (tests/stream_loop_tests.cpp), because "which clip
// does the transport need next" is exactly the sort of arithmetic a loop makes
// subtle: behind the playhead is not the same as never again when the pass
// repeats.
#pragma once

#include "../model/types.h"

namespace daw {

// A sort key: smaller starts filling earlier. Clips the transport needs soonest
// get the smallest keys, and a clip that will never play again in this
// transport's future sorts last — after every clip that will.
//
// Frame arithmetic only; `kNever` just has to exceed any real distance, and it
// is added to the clip's own start so the never-played clips keep a stable
// order among themselves.
inline constexpr Frame kStreamNeverKey = (Frame)1 << 50;

inline Frame StreamDueKey(Frame clipStart, Frame clipEnd, Frame playhead,
                          bool looping, Frame loopStart, Frame loopEnd) {
    const bool loops = looping && loopEnd > loopStart;
    if (!loops) {
        if (clipEnd <= playhead)   return kStreamNeverKey + clipStart;  // behind
        if (clipStart <= playhead) return 0;                 // playing right now
        return clipStart - playhead;                         // ahead of us
    }
    // Looping: the transport visits [playhead, loopEnd) once and [loopStart,
    // loopEnd) for ever after. A clip outside that future never sounds again —
    // however soon its own start looks in absolute terms (a clip past the loop
    // end is unreachable, and one before the loop start is reachable only while
    // the transport is still on its first pass).
    const bool inLoop = clipEnd > loopStart && clipStart < loopEnd;
    if (!inLoop) {
        if (clipEnd <= playhead || playhead >= loopStart)
            return kStreamNeverKey + clipStart;    // out of the loop's reach
        return clipStart - playhead;               // first pass, still ahead
    }
    if (clipStart <= playhead && clipEnd > playhead) return 0;   // right now
    if (clipStart > playhead) return clipStart - playhead;       // later this pass
    // Behind the playhead but inside the loop: it comes round again after the
    // wrap, at its own distance from the loop start.
    return (loopEnd - playhead)
           + (clipStart > loopStart ? clipStart - loopStart : 0);
}

} // namespace daw
