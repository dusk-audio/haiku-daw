#include "Gate.h"

#include <algorithm>
#include <cmath>

namespace daw {

Gate::Gate(double thresholdDb, double ratio,
           double attackMs, double releaseMs, double rangeDb)
    : fThresholdDb(thresholdDb), fRatio(ratio),
      fAttackMs(attackMs), fReleaseMs(releaseMs), fRangeDb(rangeDb) {
    Recompute();
}

void Gate::SetParams(double thresholdDb, double ratio,
                     double attackMs, double releaseMs, double rangeDb) {
    fThresholdDb = thresholdDb;
    fRatio       = ratio;
    fAttackMs    = attackMs;
    fReleaseMs   = releaseMs;
    fRangeDb     = rangeDb;
    Recompute();
}

void Gate::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    Recompute();
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
}

void Gate::Process(float* stereo, int frames) {
    const double ratio    = fRatio < 1.0 ? 1.0 : fRatio;
    const double slope    = ratio - 1.0;         // dB attenuation per dB under
    const double threshDb = fThresholdDb;
    const double floorGain = fRangeFloor;

    for (int i = 0; i < frames; i++) {
        const double l = stereo[i * 2 + 0];
        const double r = stereo[i * 2 + 1];

        // Stereo-linked peak detection (max of the two channels' magnitudes).
        const double peak = std::max(std::fabs(l), std::fabs(r));
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
}

void Gate::SetParam(int slot, float v) {
    double t = fThresholdDb, r = fRatio, a = fAttackMs, rl = fReleaseMs, rg = fRangeDb;
    switch (slot) { case 0: t = v; break; case 1: r = v; break; case 2: a = v; break;
                    case 3: rl = v; break; case 4: rg = v; break; default: return; }
    SetParams(t, r, a, rl, rg);
}

} // namespace daw
