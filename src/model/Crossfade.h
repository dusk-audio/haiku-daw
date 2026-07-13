// Crossfade — derive effective clip fades from timeline overlaps.
//
// When two clips on a track overlap in time, we auto-crossfade: the earlier
// clip fades out and the later fades in across the overlap. Rather than mutate
// the model, the renderer asks this helper for each clip's *effective* fade
// lengths — the clip's own fades combined (via max) with any auto-crossfade
// implied by overlapping its neighbor. Linear fades; for the common case of
// butting/overlapping takes this keeps the sum well-behaved.
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
    Frame fadeIn  = 0;
    Frame fadeOut = 0;
};

inline std::vector<ClipFades> ComputeCrossfades(const std::vector<Clip>& clips) {
    std::vector<ClipFades> out(clips.size());
    for (std::size_t i = 0; i < clips.size(); i++)
        out[i] = { clips[i].fadeInFrames, clips[i].fadeOutFrames };

    for (std::size_t i = 0; i + 1 < clips.size(); i++) {
        const Clip& a = clips[i];
        const Clip& b = clips[i + 1];
        const Frame aEnd = a.startFrame + a.lengthFrames;
        if (b.startFrame >= aEnd) continue;            // no overlap

        Frame overlap = aEnd - b.startFrame;           // >0
        overlap = std::min(overlap, a.lengthFrames);
        overlap = std::min(overlap, b.lengthFrames);
        if (overlap <= 0) continue;

        out[i].fadeOut    = std::max(out[i].fadeOut, overlap);
        out[i + 1].fadeIn = std::max(out[i + 1].fadeIn, overlap);
    }
    return out;
}

} // namespace daw
