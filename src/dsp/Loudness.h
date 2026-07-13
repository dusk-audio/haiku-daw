// Loudness — ITU-R BS.1770-4 / EBU R128 loudness meter (LUFS) plus a
// BS.1770 true-peak (dBTP) detector. This is a *measurement sink*, not an
// IEffect: Process() reads the audio and never modifies it.
//
// It reports:
//   - Momentary loudness  (400 ms sliding window, K-weighted)
//   - Short-term loudness  (3 s sliding window, K-weighted)
//   - Integrated loudness  (gated program loudness: -70 LUFS absolute gate,
//                           then -10 LU relative gate over 400 ms blocks)
//   - True peak            (4x oversampled, dBTP; always >= sample peak)
//
// K-weighting is two cascaded Direct-Form-I biquads (same recurrence style as
// daw::Biquad): a high-shelf (~+4 dB, ~1.68 kHz) followed by a ~38 Hz
// high-pass. The coefficients are the BS.1770-4 analog prototypes mapped to
// the running sample rate via the pre-warped bilinear transform (so the meter
// is correct at 44.1 kHz, 96 kHz, ... not just 48 kHz). See Loudness.cpp.
//
// Kit-free (STL only), C++17, host-testable.
#pragma once

#include <cstddef>
#include <deque>
#include <vector>

namespace daw {

class Loudness {
public:
    Loudness() = default;

    // Build the K-weighting filters and window/ring buffers for this rate and
    // clear all state. Safe to call again to change sample rate.
    void Prepare(double sampleRate);

    // Feed interleaved stereo audio. Measurement only; `stereo` is const and
    // is never written back.
    void Process(const float* stereo, int frames);

    // Loudness readings, in LUFS (== LKFS). Silence / no data reads a finite
    // floor (kSilenceLufs), never -inf.
    float MomentaryLufs() const;   // 400 ms window
    float ShortTermLufs() const;   // 3 s window
    float IntegratedLufs() const;  // gated, whole-program

    // Maximum true peak observed so far, in dBTP. Always >= the raw sample
    // peak. Reads kSilenceDb for silence.
    float TruePeakDb() const;

    // Clear all history (filters, windows, gating blocks, peak hold).
    void Reset();

    // Sum of the true-peak FIR coefficients for a phase (its DC gain). Exposed
    // for validation: every phase must be ~1.0 (unity passband at DC).
    double TpPhaseDcGain(int phase) const;

    // Finite floor values returned in place of -inf.
    static constexpr float kSilenceLufs = -100.0f;
    static constexpr float kSilenceDb   = -100.0f;

private:
    // One Direct-Form-I biquad, normalized (a0 folded in), independent state
    // per stereo channel.
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1[2] = {0, 0}, x2[2] = {0, 0};
        double y1[2] = {0, 0}, y2[2] = {0, 0};
        void   ResetState();
        double Tick(double x, int ch);
    };

    void BuildKWeighting(double sampleRate);
    void BuildTruePeak();   // generate the DC-normalized 4x polyphase FIR

    // Convert a summed-channel mean square to LUFS, clamped to kSilenceLufs.
    static float MeanSquareToLufs(double meanSquare);

    double fSampleRate = 48000.0;

    // K-weighting: high-shelf then high-pass, cascaded.
    Biquad fShelf;
    Biquad fHighPass;

    // Momentary (400 ms) and short-term (3 s) sliding windows hold per-sample
    // summed-channel K-weighted power (kL^2 + kR^2). meanSquare = sum/size.
    std::vector<double> fMomRing;
    std::vector<double> fShortRing;
    std::size_t fMomPos = 0, fShortPos = 0;
    double      fMomSum = 0.0, fShortSum = 0.0;

    // Integrated: overlapping 400 ms gating blocks stepped every 100 ms. We
    // accumulate summed-channel power over 100 ms sub-blocks; the last four
    // sub-blocks form one 400 ms gating block. We store each gating block's
    // mean square for later gating.
    int    fSubLen   = 4800;  // samples per 100 ms sub-block
    int    fSubPos   = 0;     // samples into current sub-block
    double fSubAccum = 0.0;   // summed-channel power in current sub-block
    std::deque<double> fSubHistory;      // recent sub-block power sums (last 4)
    std::vector<double> fBlockMeanSq;    // 400 ms gating-block mean squares

    // True peak: 4x polyphase FIR oversampler, per-channel 12-sample history.
    static constexpr int kOs   = 4;
    static constexpr int kTaps = 12;
    double fTpCoeffs[kOs][kTaps] = {{0}};   // generated in BuildTruePeak()
    double fTpHistory[2][kTaps] = {{0}};
    int    fTpPos[2] = {0, 0};
    double fMaxTruePeak = 0.0;  // linear
};

} // namespace daw
