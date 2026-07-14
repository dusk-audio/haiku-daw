#include "Compressor.h"

#include <algorithm>
#include <cmath>

namespace daw {

Compressor::Compressor(double thresholdDb, double ratio,
                       double attackMs, double releaseMs, double makeupDb)
    : fThresholdDb(thresholdDb), fRatio(ratio),
      fAttackMs(attackMs), fReleaseMs(releaseMs), fMakeupDb(makeupDb) {
    Recompute();
}

void Compressor::SetParams(double thresholdDb, double ratio,
                           double attackMs, double releaseMs, double makeupDb) {
    fThresholdDb = thresholdDb;
    fRatio       = ratio;
    fAttackMs    = attackMs;
    fReleaseMs   = releaseMs;
    fMakeupDb    = makeupDb;
    Recompute();
}

void Compressor::Prepare(double sampleRate) {
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

void Compressor::Recompute() {
    fAttackCoef  = SmoothCoef(fAttackMs  * 0.001, fSampleRate);
    fReleaseCoef = SmoothCoef(fReleaseMs * 0.001, fSampleRate);
    fMakeupLin   = std::pow(10.0, fMakeupDb / 20.0);
}

void Compressor::Reset() {
    // Envelope tracks the applied GAIN (1 = no reduction), as in the donor's
    // DigitalCompressor detector.
    fEnv = 1.0;
}

void Compressor::Process(float* stereo, int frames) {
    const double ratio    = fRatio < 1.0 ? 1.0 : fRatio;
    const double slope    = 1.0 - 1.0 / ratio;   // dB reduction per dB over
    const double threshDb = fThresholdDb;

    for (int i = 0; i < frames; i++) {
        const double l = stereo[i * 2 + 0];
        const double r = stereo[i * 2 + 1];

        // Stereo-linked peak detection (max of the two channels' magnitudes).
        const double peak = std::max(std::fabs(l), std::fabs(r));
        const double detDb = 20.0 * std::log10(std::max(peak, 1.0e-5));

        // Hard-knee gain computer (DigitalCompressor form): reduction in dB
        // above threshold, converted to a linear target gain.
        double reductionDb = 0.0;
        if (detDb > threshDb)
            reductionDb = (detDb - threshDb) * slope;
        const double targetGain = std::pow(10.0, -reductionDb / 20.0);

        // Smooth the GAIN: attack when the gain must fall (target below the
        // current envelope), release when it recovers — the donor's topology.
        const double coef = (targetGain < fEnv) ? fAttackCoef : fReleaseCoef;
        fEnv = coef * fEnv + (1.0 - coef) * targetGain;
        if (fEnv < 1.0e-4) fEnv = 1.0e-4;
        if (fEnv > 1.0)    fEnv = 1.0;

        const double g = fEnv * fMakeupLin;
        stereo[i * 2 + 0] = static_cast<float>(l * g);
        stereo[i * 2 + 1] = static_cast<float>(r * g);
    }
}

void Compressor::SetParam(int slot, float v) {
    double t = fThresholdDb, r = fRatio, a = fAttackMs, rl = fReleaseMs, m = fMakeupDb;
    switch (slot) { case 0: t = v; break; case 1: r = v; break; case 2: a = v; break;
                    case 3: rl = v; break; case 4: m = v; break; default: return; }
    SetParams(t, r, a, rl, m);
}

} // namespace daw
