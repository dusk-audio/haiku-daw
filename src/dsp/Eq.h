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
    // Live input spectrum (magnitude dB per FFT bin, low->high) for the analyzer
    // overlay. The RT thread only CAPTURES input frames; this call runs the FFT
    // itself on the UI thread (it is polled from the meter loop), keeping the
    // transform off the audio thread.
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
    void ComputeBand(int b);   // -> target coefficients for band b
    void SnapCoeffs();         // live = target for all bands (no glide)
    void CaptureSpectrumSample(float mono);   // RT: buffer input; publish a frame

    double fSampleRate = 48000.0;

    // FFT analyzer handoff. The RT thread captures the EQ input into fCap and,
    // each time a full frame fills, copies it into the buffer the UI is NOT
    // reading (fCapSnap[!published]) and bumps fSnapSeq. The UI thread's
    // Spectrum() runs the actual FFT lazily on the latest published snapshot, so
    // the O(N log N) transform stays OFF the audio thread — the RT side only
    // does a 512-float copy + two atomic stores. Double-buffered + versioned so
    // the reader never sees a half-written frame.
    float  fCap[kFftSize] = {};      // RT capture ring
    int    fCapPos = 0;              // RT
    float  fWin[kFftSize] = {};      // Hann window (built in Prepare)
    float  fCapSnap[2][kFftSize] = {{}};  // RT-published frame snapshots
    std::atomic<int>      fSnapIdx{-1};   // published buffer index (-1 = none)
    std::atomic<unsigned> fSnapSeq{0};    // frame version (bumped on publish)

    // UI-thread FFT cache — mutable: Spectrum() is logically const but derives
    // and caches the transform of the newest snapshot. Touched only on the UI
    // thread (the meter poll), never from Process().
    mutable float    fMag[kBins] = {};   // last computed magnitude spectrum (dB)
    mutable int      fMagCount = 0;      // bins in fMag (0 before first FFT)
    mutable unsigned fMagSeq   = 0;      // fSnapSeq the cache was computed from
    mutable double   fRe[kFftSize] = {}; // FFT scratch (UI thread only)
    mutable double   fIm[kFftSize] = {};

    float  fFreq[kBands];
    float  fGainDb[kBands];
    float  fQ[kBands];

    // Live normalized biquad coefficients per band (a0 folded in) used by
    // Process, and the targets they glide toward. Automation moves the targets;
    // Process one-pole-smooths the live coeffs per sample so a coefficient step
    // can't click (zipper). Converged (no automation) => live == target, so the
    // static response is unchanged.
    double fB0[kBands], fB1[kBands], fB2[kBands], fA1[kBands], fA2[kBands];
    double fTB0[kBands], fTB1[kBands], fTB2[kBands], fTA1[kBands], fTA2[kBands];
    double fCoefSmooth = 0.0;   // per-sample glide factor (0 = snap), set in Prepare

    // Per-band Direct Form I state: [band][channel] x[n-1],x[n-2],y[n-1],y[n-2].
    double fX1[kBands][2], fX2[kBands][2], fY1[kBands][2], fY2[kBands][2];
};

} // namespace daw
