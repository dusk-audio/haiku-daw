#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace daw {

Gate::Gate(double thresholdDb, double ratio,
           double attackMs, double releaseMs, double rangeDb)
    : fThresholdDb(thresholdDb), fRatio(ratio),
      fAttackMs(attackMs), fReleaseMs(releaseMs), fRangeDb(rangeDb) {
    Recompute();
    fFloorCur = fRangeFloor;
}

void Gate::SetParams(double thresholdDb, double ratio,
                     double attackMs, double releaseMs, double rangeDb) {
    fThresholdDb = thresholdDb;
    fRatio       = ratio;
    fAttackMs    = attackMs;
    fReleaseMs   = releaseMs;
    fRangeDb     = rangeDb;
    Recompute();
    if (fCoefSmooth <= 0.0)   // not yet Prepared: apply range floor immediately
        fFloorCur = fRangeFloor;
}

// ~10 ms glide on the range floor so an automated range step ramps instead of
// stepping the closed-gate gain. Threshold/ratio already reach the output
// through the attack/release gain envelope, so only the floor needs smoothing.
static constexpr double kFloorSmoothMs = 10.0;

void Gate::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    fCoefSmooth = 1.0 - std::exp(-1.0 / (kFloorSmoothMs * 0.001 * fSampleRate));
    Recompute();
    fFloorCur = fRangeFloor;   // start on-target
    Reset();
}

// One-pole smoothing coefficient for a time constant in seconds — the exact
// form the user's DigitalCompressor uses: coef = exp(-1 / (t * Fs)). A larger
// coef (longer time) means the gain moves toward its target more slowly.
static double SmoothCoef(double seconds, double sampleRate) {
    const double t = seconds > 1.0e-5 ? seconds : 1.0e-5;   // clamp like the donor
    return std::exp(-1.0 / (t * sampleRate));
}

void Gate::Recompute() {
    fAttackCoef  = SmoothCoef(fAttackMs  * 0.001, fSampleRate);
    fReleaseCoef = SmoothCoef(fReleaseMs * 0.001, fSampleRate);
    // Range floor: the lowest linear gain when the gate is fully closed.
    const double range = fRangeDb < 0.0 ? 0.0 : fRangeDb;
    fRangeFloor = std::pow(10.0, -range / 20.0);
}

void Gate::Reset() {
    // Envelope tracks the applied GAIN (1 = fully open), as in the donor's
    // detector. Start open so a signal that begins above threshold passes.
    fEnv = 1.0;
    fKey.block = nullptr;   // a stale key must not survive a seek/reload
}

void Gate::Process(float* stereo, int frames) {
    const double ratio    = fRatio < 1.0 ? 1.0 : fRatio;
    const double slope    = ratio - 1.0;         // dB attenuation per dB under
    const double threshDb = fThresholdDb;

    // External key: extKey on AND a key actually routed this block (fail soft
    // to internal detection otherwise — never a stale key). The gain still
    // lands on `stereo`; only the DETECTOR moves to the key, which is what
    // lets one track open or close another. The key reduction is the same one
    // the internal path runs (SidechainKey.h), so the two cannot drift.
    const bool   keyed = fExtKey && fKey.Active();
    const float* det   = keyed ? fKey.block : stereo;

    for (int i = 0; i < frames; i++) {
        // Glide the range floor toward its target so an automated range step
        // can't step the closed-gate gain; the envelope (fEnv) is already smoothed.
        fFloorCur += (fRangeFloor - fFloorCur) * fCoefSmooth;
        const double floorGain = fFloorCur;

        const double l = stereo[i * 2 + 0];
        const double r = stereo[i * 2 + 1];

        const double peak  = DetectorLevel(det + i * 2, keyed);
        const double detDb = 20.0 * std::log10(std::max(peak, 1.0e-9));

        // Downward-expander gain computer (mirror of the compressor's): when the
        // detected level is BELOW threshold, attenuate by (threshold - level) *
        // (ratio - 1) dB, clamped to the range. Above threshold => unity.
        double reductionDb = 0.0;
        if (detDb < threshDb)
            reductionDb = (threshDb - detDb) * slope;
        double targetGain = std::pow(10.0, -reductionDb / 20.0);
        if (targetGain < floorGain) targetGain = floorGain;
        if (targetGain > 1.0)       targetGain = 1.0;

        // Smooth the GAIN: attack when the gate opens (target above the current
        // envelope, gain rising), release when it closes (gain falling) — the
        // gate convention, opposite to the compressor's.
        const double coef = (targetGain > fEnv) ? fAttackCoef : fReleaseCoef;
        fEnv = coef * fEnv + (1.0 - coef) * targetGain;
        if (fEnv < floorGain) fEnv = floorGain;
        if (fEnv > 1.0)       fEnv = 1.0;

        stereo[i * 2 + 0] = static_cast<float>(l * fEnv);
        stereo[i * 2 + 1] = static_cast<float>(r * fEnv);
    }

    // The key belonged to this one call (see SetSidechain's contract). Dropping
    // it here means an insert whose host stops supplying one falls back to
    // internal detection rather than detecting on a rewritten buffer.
    fKey.block = nullptr;
}

void Gate::SetParam(int slot, float v) {
    if (slot == kExtKeySlot) {   // appended slot: the external-key toggle
        fExtKey = v >= 0.5f;
        return;
    }
    double t = fThresholdDb, r = fRatio, a = fAttackMs, rl = fReleaseMs, rg = fRangeDb;
    switch (slot) { case 0: t = v; break; case 1: r = v; break; case 2: a = v; break;
                    case 3: rl = v; break; case 4: rg = v; break; default: return; }
    SetParams(t, r, a, rl, rg);
}

} // namespace daw
