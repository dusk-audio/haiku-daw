#include "Automation.h"

#include <algorithm>

namespace daw {

void AutomationLane::AddPoint(Frame frame, float value) {
    // Find the first point at or after `frame`. std::lower_bound keeps the
    // vector sorted with a single insertion (or overwrite on an exact match).
    auto it = std::lower_bound(
        fPoints.begin(), fPoints.end(), frame,
        [](const AutoPoint& p, Frame f) { return p.frame < f; });

    if (it != fPoints.end() && it->frame == frame) {
        it->value = value;          // overwrite the existing point at this frame
    } else {
        fPoints.insert(it, AutoPoint{frame, value});
    }
}

bool AutomationLane::RemovePoint(size_t index) {
    if (index >= fPoints.size())
        return false;
    fPoints.erase(fPoints.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

float AutomationLane::ValueAt(Frame frame, float defaultValue) const {
    if (fPoints.empty())
        return defaultValue;

    // Hold at the ends.
    if (frame <= fPoints.front().frame)
        return fPoints.front().value;
    if (frame >= fPoints.back().frame)
        return fPoints.back().value;

    // Find the first point strictly after `frame`; the one before it is the
    // left neighbour. Both are guaranteed to exist given the range checks above.
    auto hi = std::upper_bound(
        fPoints.begin(), fPoints.end(), frame,
        [](Frame f, const AutoPoint& p) { return f < p.frame; });
    const AutoPoint& b = *hi;
    const AutoPoint& a = *(hi - 1);

    if (b.frame == a.frame)         // defensive: coincident frames
        return b.value;

    const double t = static_cast<double>(frame - a.frame) /
                     static_cast<double>(b.frame - a.frame);
    return static_cast<float>(a.value + t * (b.value - a.value));
}

} // namespace daw
