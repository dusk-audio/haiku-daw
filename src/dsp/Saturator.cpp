#include "Saturator.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

// Bounded cubic soft clip — the tape-echo tape saturation curve (verbatim from
// TapeEchoDSP::softClip). tanh-like and branch-light; monotonic on [-3,3] with
// |out| <= 1 (softClip(3) == 1). This is the sublinear transfer curve.
inline float SoftClip(float x) noexcept {
    x = std::clamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// FET preamp shape — the tape-echo preampShape (verbatim). The 0.08*x^2 term is
// the mild asymmetry that generates even harmonics (the bias/analog character);
// it introduces DC, removed by the DC blocker in Process().
inline float PreampShape(float x) noexcept {
    x = std::clamp(x, -2.5f, 2.5f);
    return SoftClip(x + 0.08f * x * x);
}

} // namespace

Saturator::Saturator(double drive, double mix, double outputTrimDb)
    : fDrive(drive), fMix(mix), fOutputTrimDb(outputTrimDb) {
    Recompute();
}

void Saturator::SetParams(double drive, double mix, double outputTrimDb) {
    fDrive        = drive;
    fMix          = mix;
    fOutputTrimDb = outputTrimDb;
    Recompute();
}

void Saturator::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    Recompute();
    Reset();
}

void Saturator::Recompute() {
    const double d = std::clamp(fDrive, 0.0, 1.0);

    // Donor drive taper (TapeEchoDSP: 0.4 + 2.6*knob^2) — clean at the bottom,
    // saturated at the top.
    fDriveMult = static_cast<float>(0.4 + 2.6 * d * d);

    // Peak/loudness compensation (donor: driveComp = 1 / softClip(drive)) so the
    // saturated peak lands near unity as drive rises.
    const float sc = SoftClip(fDriveMult);
    fDriveComp = (sc > 1.0e-6f) ? 1.0f / sc : 1.0f;

    fTrimLin = static_cast<float>(std::pow(10.0, fOutputTrimDb / 20.0));

    // DC blocker pole from a ~20 Hz corner that tracks the sample rate
    // (DuskFilters::DCBlocker::setSampleRate).
    const double fs = fSampleRate > 0.0 ? fSampleRate : 48000.0;
    fDcR = static_cast<float>(std::exp(-2.0 * 3.14159265358979323846 * 20.0 / fs));
}

void Saturator::Reset() {
    // Clear the per-channel DC-blocker memory so a seek can't bleed stale DC.
    fDcX1L = fDcY1L = 0.0f;
    fDcX1R = fDcY1R = 0.0f;
}

void Saturator::Process(float* stereo, int frames) {
    const float driveMult = fDriveMult;
    const float driveComp = fDriveComp;
    const float trim      = fTrimLin;
    const float mix       = static_cast<float>(std::clamp(fMix, 0.0, 1.0));
    const float dryGain   = 1.0f - mix;
    const float R         = fDcR;

    for (int i = 0; i < frames; i++) {
        const float xl = stereo[i * 2 + 0];
        const float xr = stereo[i * 2 + 1];

        // --- left ---------------------------------------------------------
        float sl = PreampShape(xl * driveMult);   // asymmetric tape/FET sat
        {                                          // DC blocker (removes x^2 DC)
            const float y = sl - fDcX1L + R * fDcY1L;
            fDcX1L = sl;
            fDcY1L = y;
            sl = y;
        }
        const float wetL = sl * driveComp * trim;
        stereo[i * 2 + 0] = xl * dryGain + wetL * mix;

        // --- right --------------------------------------------------------
        float sr = PreampShape(xr * driveMult);
        {
            const float y = sr - fDcX1R + R * fDcY1R;
            fDcX1R = sr;
            fDcY1R = y;
            sr = y;
        }
        const float wetR = sr * driveComp * trim;
        stereo[i * 2 + 1] = xr * dryGain + wetR * mix;
    }
}

void Saturator::SetParam(int slot, float v) {
    double d = fDrive, m = fMix, o = fOutputTrimDb;
    switch (slot) { case 0: d = v; break; case 1: m = v; break; case 2: o = v; break;
                    default: return; }
    SetParams(d, m, o);
}

} // namespace daw
