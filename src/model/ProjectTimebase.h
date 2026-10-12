// ProjectTimebase — rescaling every project-rate frame in the model.
//
// A project's frames ARE samples at its own rate: clip positions and lengths,
// the source offset inside a clip, note and CC positions, markers, the loop and
// punch points, tempo and meter change anchors and automation breakpoints. So
// changing the sample rate (Project Settings, M3.6) is a rescale of the whole
// timeline -- a bar stays a bar and a clip stays where the user put it -- and
// this is the one place that arithmetic lives.
//
// Kit-free (STL only), so it is host-tested against every frame-bearing field:
// a field a rescale forgets silently slides in time, which is exactly the kind
// of bug a screenshot cannot catch.
#pragma once

#include "Project.h"

namespace daw {

// One frame through the rescale's own rounding (nearest frame), for callers
// that hold a position of their own (a test's expectation, the UI's playhead).
Frame ScaleFrame(Frame f, double ratio);

// Multiply every frame-valued field in `p` by `ratio`, rounding to the nearest
// frame. Values that are NOT time -- ids, gains, pans, colours, lane heights,
// the sample rate itself -- are left alone; the caller sets the rate.
//
// `ratio` of 1.0 (or anything non-finite) is a no-op.
void ScaleProjectTimebase(Project& p, double ratio);

} // namespace daw
