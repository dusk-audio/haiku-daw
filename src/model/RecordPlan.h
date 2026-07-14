// RecordPlan — pure timing/placement math for recording features.
//
// Kit-free helpers that turn a raw linear capture (the recorder writes one
// continuous file) into timeline clip placements for count-in, punch-in/out,
// and loop-record takes. Keeping the math here makes it host-testable, away
// from the Media Kit orchestration in MainWindow.
//
// All frames are timeline (project-rate) frames unless noted. A capture that
// began at `captureStart` and produced `captureLen` frames maps to the source
// file offset (frame - captureStart).
//
// Header-only, STL only.
#pragma once

#include "TempoMap.h"
#include "types.h"

#include <algorithm>
#include <vector>

namespace daw {

// Frames of count-in for `bars` bars of the meter in effect at `playhead`.
// Zero bars (or non-positive) -> 0.
inline Frame CountInFrames(const TempoMap& tm, Frame playhead, int bars) {
    if (bars <= 0) return 0;
    if (playhead < 0) playhead = 0;
    int num = 4, den = 4;
    tm.Meter(playhead, &num, &den);
    const double startBeat = tm.BeatAt(playhead);
    const Frame end = tm.FrameAt(startBeat + (double)bars * num);
    return end > playhead ? end - playhead : 0;
}

// One placed clip region cut from the linear capture.
struct TakeRegion {
    Frame startFrame   = 0;   // timeline position
    Frame sourceOffset = 0;   // offset into the captured file
    Frame lengthFrames = 0;
};

// Trim a capture [captureStart, captureStart+captureLen) to a punch range
// [punchIn, punchOut). Returns false (empty region) if they don't overlap or
// the punch range is degenerate.
inline bool PunchedTake(Frame captureStart, Frame captureLen,
                        Frame punchIn, Frame punchOut, TakeRegion* out) {
    if (captureLen <= 0 || punchOut <= punchIn) return false;
    const Frame capEnd = captureStart + captureLen;
    const Frame start  = std::max(captureStart, punchIn);
    const Frame end    = std::min(capEnd, punchOut);
    if (end <= start) return false;
    out->startFrame   = start;
    out->sourceOffset = start - captureStart;
    out->lengthFrames = end - start;
    return true;
}

// Split a linear loop-record capture into one take per loop pass. The capture
// began at `loopStart` and ran `captureLen` frames while playback looped over
// [loopStart, loopEnd). Each pass i is a clip at loopStart with source offset
// i*L; the final pass may be shorter. Empty if the loop range is degenerate.
inline std::vector<TakeRegion> LoopTakes(Frame loopStart, Frame loopEnd,
                                         Frame captureLen) {
    std::vector<TakeRegion> takes;
    const Frame L = loopEnd - loopStart;
    if (L <= 0 || captureLen <= 0) return takes;
    Frame consumed = 0;
    while (consumed < captureLen) {
        const Frame len = std::min(L, captureLen - consumed);
        TakeRegion t;
        t.startFrame   = loopStart;
        t.sourceOffset = consumed;
        t.lengthFrames = len;
        takes.push_back(t);
        consumed += len;
    }
    return takes;
}

} // namespace daw
