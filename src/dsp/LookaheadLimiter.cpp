#include "LookaheadLimiter.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {
// Aim a hair below the ceiling so float rounding in the gain multiply can never
// nudge a sample back over it.
constexpr double kSafety = 0.999;

double clampd(double v, double lo, double hi) {
    if (!std::isfinite(v)) return lo;
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

LookaheadLimiter::LookaheadLimiter(double ceilingDb, double lookaheadMs,
                                   double releaseMs, double inGainDb)
    : fCeilingDb(ceilingDb), fLookaheadMs(lookaheadMs),
      fReleaseMs(releaseMs), fInGainDb(inGainDb) {}

void LookaheadLimiter::Recompute() {
    const double ceilLin = std::pow(10.0, clampd(fCeilingDb, -60.0, 0.0) / 20.0);
    fTargetLin = ceilLin * kSafety;
    fInGainLin = std::pow(10.0, clampd(fInGainDb, -24.0, 48.0) / 20.0);
    double relSamp = clampd(fReleaseMs, 1.0, 5000.0) * fSampleRate / 1000.0;
    if (relSamp < 1.0) relSamp = 1.0;
    fAlphaR = std::exp(-1.0 / relSamp);
}

void LookaheadLimiter::Prepare(double sampleRate) {
    fSampleRate = (sampleRate > 0.0) ? sampleRate : 48000.0;

    // Look-ahead in frames, fixed for this Prepare()/Process() lifetime. Clamp to
    // [1, 1 s] so the reported latency is always real (>= 1) and the ring bounded.
    double laMs = clampd(fLookaheadMs, 0.0, 1000.0);
    double la   = std::floor(laMs * fSampleRate / 1000.0 + 0.5);
    if (la < 1.0) la = 1.0;
    if (la > fSampleRate) la = fSampleRate;
    fLatency = static_cast<int>(la);

    Recompute();

    fDelay.assign(static_cast<std::size_t>(fLatency) * 2, 0.0f);
    fWrite = 0;

    // Monotonic-deque storage: at most La+1 live entries; +1 slack.
    const std::size_t cap = static_cast<std::size_t>(fLatency) + 2;
    fMqVal.assign(cap, 0.0f);
    fMqIdx.assign(cap, 0);
    fMqHead = 0;
    fMqCount = 0;

    fCounter = 0;
    fGcur    = 1.0;
    fGrDb.store(0.0f, std::memory_order_relaxed);
}

void LookaheadLimiter::Reset() {
    std::fill(fDelay.begin(), fDelay.end(), 0.0f);
    fWrite   = 0;
    fMqHead  = 0;
    fMqCount = 0;
    fCounter = 0;
    fGcur    = 1.0;
    fGrDb.store(0.0f, std::memory_order_relaxed);
}

void LookaheadLimiter::SetParam(int slot, float value) {
    switch (slot) {
        case 0: fCeilingDb = value; Recompute(); break;
        // Look-ahead sizes the delay ring + the reported latency, both fixed at
        // Prepare(); changing it live would require a realloc and would break the
        // PDC latency contract, so we only stash it (takes effect on next Prepare).
        case 1: fLookaheadMs = value; break;
        case 2: fReleaseMs = value; Recompute(); break;
        case 3: fInGainDb = value;  Recompute(); break;
        default: break;
    }
}

void LookaheadLimiter::Process(float* x, int frames) {
    if (!x || frames <= 0 || fDelay.empty()) return;

    const std::size_t La  = static_cast<std::size_t>(fLatency);
    const std::size_t cap = fMqVal.size();
    const float       inG = static_cast<float>(fInGainLin);
    float minGain = 1.0f;

    for (int f = 0; f < frames; ++f) {
        float gl = x[f * 2 + 0] * inG;
        float gr = x[f * 2 + 1] * inG;
        // Flush non-finite input to 0: a NaN peak defeats the window-max compare
        // (NaN <= x is false) and would let the ceiling slip for La frames; an Inf
        // would emit Inf*0 = NaN downstream. Keep the limiter's output finite and
        // the ceiling guarantee intact regardless of upstream garbage.
        if (!std::isfinite(gl)) gl = 0.0f;
        if (!std::isfinite(gr)) gr = 0.0f;
        const float peak = std::max(std::fabs(gl), std::fabs(gr));
        const std::int64_t i = fCounter;

        // Push onto the monotonic-decreasing deque: drop tail entries this peak
        // dominates (they can never again be the window max), then append.
        while (fMqCount > 0) {
            const std::size_t back = (fMqHead + fMqCount - 1) % cap;
            if (fMqVal[back] <= peak) --fMqCount; else break;
        }
        const std::size_t slot = (fMqHead + fMqCount) % cap;
        fMqVal[slot] = peak;
        fMqIdx[slot] = i;
        ++fMqCount;

        // Evict the front once it falls out of the look-ahead window [i-La, i].
        while (fMqCount > 0 && fMqIdx[fMqHead] < i - static_cast<std::int64_t>(La)) {
            fMqHead = (fMqHead + 1) % cap;
            --fMqCount;
        }

        const double windowMax = fMqVal[fMqHead];
        const double gTarget =
            (windowMax > fTargetLin) ? (fTargetLin / windowMax) : 1.0;

        // Attack: snap down instantly (the drop is already La ahead of the peak
        // at the output). Release: recover toward the target with a time constant.
        if (gTarget < fGcur) fGcur = gTarget;
        else                 fGcur = gTarget + (fGcur - gTarget) * fAlphaR;
        const float g = static_cast<float>(fGcur);

        // Emit the La-old frame scaled by the current gain; store the new frame.
        const float outL = fDelay[fWrite * 2 + 0];
        const float outR = fDelay[fWrite * 2 + 1];
        fDelay[fWrite * 2 + 0] = gl;
        fDelay[fWrite * 2 + 1] = gr;
        if (++fWrite >= La) fWrite = 0;

        x[f * 2 + 0] = outL * g;
        x[f * 2 + 1] = outR * g;

        if (g < minGain) minGain = g;
        ++fCounter;
    }

    fGrDb.store((minGain < 1.0f && minGain > 0.0f)
                    ? static_cast<float>(20.0 * std::log10(minGain))
                    : 0.0f,
                std::memory_order_relaxed);
}

} // namespace daw
