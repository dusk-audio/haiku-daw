#include "RegionOps.h"

#include <cmath>

namespace daw {

namespace {
inline float Mag(const float* p, int64_t f) {
    const float l = std::fabs(p[f * 2 + 0]);
    const float r = std::fabs(p[f * 2 + 1]);
    return l > r ? l : r;
}
} // namespace

float PeakLinear(const float* interleaved, int64_t frames) {
    float peak = 0.0f;
    for (int64_t f = 0; f < frames; ++f) {
        const float m = Mag(interleaved, f);
        if (m > peak) peak = m;
    }
    return peak;
}

void ReverseStereo(float* interleaved, int64_t frames) {
    for (int64_t i = 0, j = frames - 1; i < j; ++i, --j) {
        std::swap(interleaved[i * 2 + 0], interleaved[j * 2 + 0]);
        std::swap(interleaved[i * 2 + 1], interleaved[j * 2 + 1]);
    }
}

std::vector<Span> NonSilentSpans(const float* interleaved, int64_t frames,
                                 float threshLinear, int64_t minSilence,
                                 int64_t pad) {
    // First pass: collect maximal silence runs that are long enough to be gaps.
    std::vector<Span> gaps;
    int64_t i = 0;
    while (i < frames) {
        if (Mag(interleaved, i) < threshLinear) {
            int64_t j = i;
            while (j < frames && Mag(interleaved, j) < threshLinear) ++j;
            if (j - i >= minSilence) gaps.push_back({i, j});
            i = j;
        } else {
            ++i;
        }
    }

    // Second pass: kept spans = complement of the (pad-shrunk) gaps. A gap that
    // vanishes once shrunk by pad on both sides is not a cut — audio plays
    // through it.
    std::vector<Span> kept;
    int64_t cur = 0;
    for (const Span& g : gaps) {
        const int64_t gs = g.start + pad;
        const int64_t ge = g.end - pad;
        if (ge <= gs) continue;          // too small after padding: not a cut
        if (gs > cur) kept.push_back({cur, gs});
        cur = ge;
    }
    if (cur < frames) kept.push_back({cur, frames});
    return kept;
}

} // namespace daw
