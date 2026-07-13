#include "Biquad.h"

#include <cmath>

namespace daw {

Biquad::Biquad(Type type, double freq, double q, double gainDb)
    : fType(type), fFreq(freq), fQ(q), fGainDb(gainDb) {
    Recompute();
}

void Biquad::SetParams(Type type, double freq, double q, double gainDb) {
    fType   = type;
    fFreq   = freq;
    fQ      = q;
    fGainDb = gainDb;
    Recompute();
}

void Biquad::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;
    Recompute();
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

    fB0 = b0 / a0;
    fB1 = b1 / a0;
    fB2 = b2 / a0;
    fA1 = a1 / a0;
    fA2 = a2 / a0;
}

void Biquad::Reset() {
    for (int c = 0; c < 2; c++) {
        fX1[c] = fX2[c] = 0.0;
        fY1[c] = fY2[c] = 0.0;
    }
}

void Biquad::Process(float* stereo, int frames) {
    for (int i = 0; i < frames; i++) {
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
