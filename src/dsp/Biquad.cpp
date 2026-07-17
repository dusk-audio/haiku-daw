#include "Biquad.h"

#include <cmath>

namespace daw {

// ~10 ms coefficient glide: long enough to defeat zipper noise on a stepped
// parameter, short enough not to smear an automation move audibly.
static constexpr double kCoefSmoothMs = 10.0;

Biquad::Biquad(Type type, double freq, double q, double gainDb)
    : fType(type), fFreq(freq), fQ(q), fGainDb(gainDb) {
    Recompute();
    SnapCoeffs();
}

void Biquad::SetParams(Type type, double freq, double q, double gainDb) {
    fType   = type;
    fFreq   = freq;
    fQ      = q;
    fGainDb = gainDb;
    Recompute();
    if (fCoefSmooth <= 0.0)   // not yet Prepared: apply immediately (no glide)
        SnapCoeffs();
}

void Biquad::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    fCoefSmooth = 1.0 - std::exp(-1.0 / (kCoefSmoothMs * 0.001 * fSampleRate));
    Recompute();
    SnapCoeffs();   // start on-target: no glide transient at playback start
    Reset();
}

// RBJ audio-EQ cookbook coefficients, normalized by a0.
void Biquad::Recompute() {
    const double q  = fQ > 1e-6 ? fQ : 1e-6;
    double f = fFreq;
    if (f < 1.0) f = 1.0;
    if (f > fSampleRate * 0.5 - 1.0) f = fSampleRate * 0.5 - 1.0;

    const double w0    = 2.0 * M_PI * f / fSampleRate;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double alpha = sinw0 / (2.0 * q);
    const double A     = std::pow(10.0, fGainDb / 40.0);   // peaking amplitude

    double b0, b1, b2, a0, a1, a2;
    switch (fType) {
        case Type::LowPass:
            b0 = (1.0 - cosw0) / 2.0;
            b1 =  1.0 - cosw0;
            b2 = (1.0 - cosw0) / 2.0;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha;
            break;
        case Type::HighPass:
            b0 = (1.0 + cosw0) / 2.0;
            b1 = -(1.0 + cosw0);
            b2 = (1.0 + cosw0) / 2.0;
            a0 =  1.0 + alpha;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha;
            break;
        case Type::Peaking:
        default:
            b0 =  1.0 + alpha * A;
            b1 = -2.0 * cosw0;
            b2 =  1.0 - alpha * A;
            a0 =  1.0 + alpha / A;
            a1 = -2.0 * cosw0;
            a2 =  1.0 - alpha / A;
            break;
    }

    fTB0 = b0 / a0;
    fTB1 = b1 / a0;
    fTB2 = b2 / a0;
    fTA1 = a1 / a0;
    fTA2 = a2 / a0;
}

void Biquad::SnapCoeffs() {
    fB0 = fTB0; fB1 = fTB1; fB2 = fTB2; fA1 = fTA1; fA2 = fTA2;
}

void Biquad::Reset() {
    for (int c = 0; c < 2; c++) {
        fX1[c] = fX2[c] = 0.0;
        fY1[c] = fY2[c] = 0.0;
    }
}

void Biquad::SetParam(int slot, float value) {
    // Mirror the editor/EffectDesc layout: 0 mode (0 LP,1 HP,2 Peak), 1 freq,
    // 2 Q, 3 gain dB. Recompute() is pure math (RT-safe).
    Type   type = fType;
    double freq = fFreq, q = fQ, gainDb = fGainDb;
    switch (slot) {
        case 0: type = value >= 1.5f ? Type::Peaking
                     : value >= 0.5f ? Type::HighPass : Type::LowPass; break;
        case 1: freq = value; break;
        case 2: q = value; break;
        case 3: gainDb = value; break;
        default: return;
    }
    SetParams(type, freq, q, gainDb);
}

void Biquad::Process(float* stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        // Glide live coefficients toward their target (shared across channels)
        // so a SetParam step ramps in over ~10 ms instead of clicking.
        fB0 += (fTB0 - fB0) * fCoefSmooth;
        fB1 += (fTB1 - fB1) * fCoefSmooth;
        fB2 += (fTB2 - fB2) * fCoefSmooth;
        fA1 += (fTA1 - fA1) * fCoefSmooth;
        fA2 += (fTA2 - fA2) * fCoefSmooth;
        for (int c = 0; c < 2; c++) {
            const double x = stereo[i * 2 + c];
            const double y = fB0 * x + fB1 * fX1[c] + fB2 * fX2[c]
                           - fA1 * fY1[c] - fA2 * fY2[c];
            fX2[c] = fX1[c]; fX1[c] = x;
            fY2[c] = fY1[c]; fY1[c] = y;
            stereo[i * 2 + c] = static_cast<float>(y);
        }
    }
}

} // namespace daw
