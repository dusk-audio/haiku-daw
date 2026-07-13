// AutomationLane — a single automation curve over the timeline.
//
// An automation lane is an ordered list of breakpoints (frame -> value) with
// linear interpolation between them. Reading a value at an arbitrary frame
// lerps between the two surrounding breakpoints; before the first point it
// holds the first value, after the last it holds the last, and an empty lane
// returns a caller-supplied default. This is the classic DAW automation
// envelope (volume, pan, plugin params, ...).
//
// Kit-free (STL only): builds and unit-tests on any host. The engine reads a
// value per block via ValueAt(); the UI edits points via AddPoint/RemovePoint.
// Points are kept sorted by frame at all times so reads are a simple scan/lerp.
#pragma once

#include "types.h"

#include <cstddef>
#include <vector>

namespace daw {

// Which per-track automation lane a command / UI targets.
enum class AutoLaneKind { Gain, Pan };

// One breakpoint: a value at a timeline frame. Values are unitless here; the
// caller decides what the range means (e.g. 0..1 gain, -1..1 pan).
struct AutoPoint {
    Frame frame = 0;
    float value = 0.0f;
};

class AutomationLane {
public:
    AutomationLane() = default;

    // Insert a breakpoint keeping the lane sorted by frame. If a point already
    // exists at `frame`, its value is overwritten instead of adding a duplicate.
    void AddPoint(Frame frame, float value);

    // Remove the point at index i. Returns false if i is out of range.
    bool RemovePoint(size_t index);

    size_t Count() const { return fPoints.size(); }

    // The breakpoint at index i. Caller must ensure i < Count().
    const AutoPoint& At(size_t i) const { return fPoints[i]; }

    // Interpolated value at `frame`:
    //   empty lane            -> defaultValue
    //   before the first point -> first point's value (hold)
    //   after the last point   -> last point's value (hold)
    //   between two points     -> linear interpolation
    float ValueAt(Frame frame, float defaultValue) const;

    void Clear() { fPoints.clear(); }

private:
    std::vector<AutoPoint> fPoints;  // sorted ascending by frame, unique frames
};

} // namespace daw
