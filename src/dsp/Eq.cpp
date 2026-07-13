#include "Eq.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Fixed band layout: low shelf, three peaks, high shelf.
enum BandType { LowShelf, Peak, HighShelf };
const BandType kType[Eq::kBands] = { LowShelf, Peak, Peak, Peak, HighShelf };
const double   kFreq[Eq::kBands] = { 80.0, 240.0, 750.0, 2200.0, 6500.0 };
const double   kQ[Eq::kBands]    = { 0.70, 0.90, 0.90, 0.90, 0.70 };

double ClampFreq(double fc, double sr) {
    return std::max(1.0, std::min(fc, sr * 0.4998));
}

// --- Ported from Multi-Q AnalogMatchedBiquad (amb) --------------------------
// Bandwidth-prewarped designs; coeffs returned normalized: b[0..2], a[1..2].
struct Co { double b0, b1, b2, a1, a2; };

Co Peaking(double fc, double sr, double gainDB, double Q) {
    fc = ClampFreq(fc, sr);
    Q  = std::max(0.01, Q);
    if (std::abs(gainDB) < 0.01) return {1, 0, 0, 0, 0};
    const double W0d  = 2.0 * kPi * fc / sr;
    const double bw   = fc / Q;
    const double kbw  = std::tan(kPi * std::min(bw, sr * 0.4998) / sr);
    const double A    = std::pow(10.0, gainDB / 40.0);
    const double cosW = std::cos(W0d);
    const double b0 = 1.0 + kbw * A, b2 = 1.0 - kbw * A;
    const double a0 = 1.0 + kbw / A, a2 = 1.0 - kbw / A;
    const double b1 = -2.0 * cosW,   a1 = -2.0 * cosW;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

Co LowShelfCo(double fc, double sr, double gainDB, double Q) {
    fc = ClampFreq(fc, sr);
    Q  = std::max(0.01, Q);
    if (std::abs(gainDB) < 0.01) return {1, 0, 0, 0, 0};
    const double A    = std::pow(10.0, gainDB / 40.0);
    const double k    = std::tan(kPi * fc / sr);
    const double k2   = k * k;
    const double sqA  = std::sqrt(A);
    const double cosW = (1.0 - k2) / (1.0 + k2);
    const double sinW = 2.0 * k / (1.0 + k2);
    const double alpha = sinW / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / Q - 1.0) + 2.0);
    const double b0 =  A * ((A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alpha);
    const double b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
    const double b2 =  A * ((A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alpha);
    const double a0 = (A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alpha;
    const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
    const double a2 = (A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alpha;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

Co HighShelfCo(double fc, double sr, double gainDB, double Q) {
    fc = ClampFreq(fc, sr);
    Q  = std::max(0.01, Q);
    if (std::abs(gainDB) < 0.01) return {1, 0, 0, 0, 0};
    const double A    = std::pow(10.0, gainDB / 40.0);
    const double sqA  = std::sqrt(A);
    const double k    = std::tan(kPi * fc / sr);
    const double k2   = k * k;
    const double cosW = (1.0 - k2) / (1.0 + k2);
    const double sinW = 2.0 * k / (1.0 + k2);
    const double alpha = sinW / 2.0 * std::sqrt((A + 1.0 / A) * (1.0 / Q - 1.0) + 2.0);
    const double b0 =  A * ((A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
    const double b2 =  A * ((A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alpha;
    const double a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cosW);
    const double a2 = (A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alpha;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

} // namespace

Eq::Eq(float g0, float g1, float g2, float g3, float g4) {
    fGainDb[0] = g0; fGainDb[1] = g1; fGainDb[2] = g2;
    fGainDb[3] = g3; fGainDb[4] = g4;
    ComputeCoeffs();
    Reset();
}

void Eq::SetGains(float g0, float g1, float g2, float g3, float g4) {
    fGainDb[0] = g0; fGainDb[1] = g1; fGainDb[2] = g2;
    fGainDb[3] = g3; fGainDb[4] = g4;
    ComputeCoeffs();
}

void Eq::Prepare(double sampleRate) {
    if (sampleRate > 0) fSampleRate = sampleRate;
    ComputeCoeffs();
    Reset();
}

void Eq::ComputeCoeffs() {
    for (int b = 0; b < kBands; b++) {
        Co c;
        switch (kType[b]) {
            case LowShelf:  c = LowShelfCo(kFreq[b], fSampleRate, fGainDb[b], kQ[b]); break;
            case HighShelf: c = HighShelfCo(kFreq[b], fSampleRate, fGainDb[b], kQ[b]); break;
            case Peak:
            default:        c = Peaking(kFreq[b], fSampleRate, fGainDb[b], kQ[b]); break;
        }
        fB0[b] = c.b0; fB1[b] = c.b1; fB2[b] = c.b2; fA1[b] = c.a1; fA2[b] = c.a2;
    }
}

void Eq::Reset() {
    for (int b = 0; b < kBands; b++)
        for (int ch = 0; ch < 2; ch++) {
            fX1[b][ch] = fX2[b][ch] = 0.0;
            fY1[b][ch] = fY2[b][ch] = 0.0;
        }
}

void Eq::Process(float* stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = stereo[i * 2 + ch];
            for (int b = 0; b < kBands; b++) {
                const double y = fB0[b] * x + fB1[b] * fX1[b][ch] + fB2[b] * fX2[b][ch]
                               - fA1[b] * fY1[b][ch] - fA2[b] * fY2[b][ch];
                fX2[b][ch] = fX1[b][ch]; fX1[b][ch] = x;
                fY2[b][ch] = fY1[b][ch]; fY1[b][ch] = y;
                x = y;   // series: feed into the next band
            }
            stereo[i * 2 + ch] = static_cast<float>(x);
        }
    }
}

} // namespace daw
