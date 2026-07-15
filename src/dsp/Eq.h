// Eq — a 5-band parametric EQ (low shelf, 3 peaks, high shelf), one of the
// built-in effects.
//
// Each band has adjustable frequency, gain (dB), and Q; the band TYPE is fixed
// by position (0 = low shelf, 1-3 = peaks, 4 = high shelf). Coefficient math is
// ported from the user's Multi-Q AnalogMatchedBiquad ("amb") core (bandwidth-
// prewarped shelves/peaks, NOT RBJ-alpha) so the tone matches their plugin.
// Direct Form I, independent state per stereo channel; coefficients recompute
// in Prepare()/SetBand, Process() is arithmetic on preallocated state.
//
// Kit-free (STL only), host-testable.
#pragma once

#include "IEffect.h"

#include <atomic>

namespace daw {

class Eq : public IEffect {
public:
    static constexpr int kBands = 5;

    Eq();

    // Set one band's frequency (Hz), gain (dB), and Q. Recomputes coeffs.
    void SetBand(int band, float freqHz, float gainDb, float q);

    void Prepare(double sampleRate) override;
    void Process(float* stereo, int frames) override;
    void Reset() override;
    void SetParam(int slot, float value) override;   // band*3 + {freq,gain,Q}
    // Live input spectrum (magnitude dB per FFT bin, low->high). For the FFT
    // analyzer overlay; RT-writer, UI-reader.
    int Spectrum(float* magDb, int maxBins) const override;
    const char* Name() const override { return "EQ"; }

    static constexpr int kFftSize = 512;
    static constexpr int kBins    = kFftSize / 2;   // usable magnitude bins

    // Band parameters (for a response-graph UI).
    float BandFreq(int b)   const { return fFreq[b]; }
    float BandGainDb(int b) const { return fGainDb[b]; }
    float BandQ(int b)      const { return fQ[b]; }

    // Summed magnitude of the 5-band cascade at a frequency, in dB. Used to
    // draw the frequency-response curve. Kit-free.
    float MagnitudeResponseDb(float freqHz) const;

private:
    void ComputeBand(int b);
    void PushSpectrumSample(float mono);   // capture input; run FFT when full

    double fSampleRate = 48000.0;

    // FFT analyzer state: capture the EQ input, window + transform when the
    // buffer fills, and store magnitudes (dB) for the editor overlay.
    float  fCap[kFftSize] = {};
    int    fCapPos = 0;
    float  fWin[kFftSize] = {};      // Hann window (built in Prepare)
    float  fMag[kBins] = {};         // last magnitude spectrum (dB)
    std::atomic<int> fMagCount{0};   // bins available (0 before first FFT)
    double fRe[kFftSize] = {};       // FFT scratch
    double fIm[kFftSize] = {};

    float  fFreq[kBands];
    float  fGainDb[kBands];
    float  fQ[kBands];

    // Normalized biquad coefficients per band (a0 folded in).
    double fB0[kBands], fB1[kBands], fB2[kBands], fA1[kBands], fA2[kBands];

    // Per-band Direct Form I state: [band][channel] x[n-1],x[n-2],y[n-1],y[n-2].
    double fX1[kBands][2], fX2[kBands][2], fY1[kBands][2], fY2[kBands][2];
};

} // namespace daw
