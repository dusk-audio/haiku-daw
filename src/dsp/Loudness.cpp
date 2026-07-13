#include "Loudness.h"

#include <algorithm>
#include <cmath>

namespace daw {

// ---------------------------------------------------------------------------
// Direct-Form-I biquad tick (same recurrence as daw::Biquad).
// ---------------------------------------------------------------------------
void Loudness::Biquad::ResetState() {
    for (int c = 0; c < 2; ++c) {
        x1[c] = x2[c] = 0.0;
        y1[c] = y2[c] = 0.0;
    }
}

double Loudness::Biquad::Tick(double x, int ch) {
    const double y = b0 * x + b1 * x1[ch] + b2 * x2[ch]
                   - a1 * y1[ch] - a2 * y2[ch];
    x2[ch] = x1[ch]; x1[ch] = x;
    y2[ch] = y1[ch]; y1[ch] = y;
    return y;
}

// ---------------------------------------------------------------------------
// True-peak 4x oversampling FIR (generated in BuildTruePeak()).
//
// A 4-phase, 12-tap polyphase interpolation FIR built from a Hann-windowed
// sinc, then DC-normalized so every phase has unit passband gain at DC. Phase 0
// is forced to an exact identity (the original delayed sample), so the reported
// true peak is always >= the raw sample peak. Phases 1..3 reconstruct the
// inter-sample values at the 1/4, 1/2, 3/4 offsets. (The earlier hard-coded
// table had a bad phase-2 — antisymmetric, DC gain 0.862 — so it under-read
// half-sample peaks; generating + normalizing fixes all four phases.)
// ---------------------------------------------------------------------------
namespace {
// EBU R128 gating thresholds.
constexpr double kAbsoluteGateLufs = -70.0;  // absolute gate
constexpr double kRelativeGateLu   = -10.0;  // relative to gated mean
} // namespace

void Loudness::BuildTruePeak() {
    const double pi = 3.14159265358979323846;
    const int center = kTaps / 2 - 1;   // 5 for 12 taps (identity delay)

    for (int p = 0; p < kOs; ++p) {
        const double d = static_cast<double>(p) / kOs;   // 0, .25, .5, .75
        double sum = 0.0;
        for (int t = 0; t < kTaps; ++t) {
            const double x = static_cast<double>(t) - center - d;
            const double s = (std::fabs(x) < 1e-9)
                             ? 1.0 : std::sin(pi * x) / (pi * x);
            const double w = 0.5 - 0.5 * std::cos(2.0 * pi * (t + 0.5) / kTaps);
            fTpCoeffs[p][t] = s * w;
            sum += fTpCoeffs[p][t];
        }
        if (std::fabs(sum) > 1e-12)          // normalize to unit DC gain
            for (int t = 0; t < kTaps; ++t)
                fTpCoeffs[p][t] /= sum;
    }
    // Phase 0 = exact identity so the true peak never reads below the sample.
    for (int t = 0; t < kTaps; ++t)
        fTpCoeffs[0][t] = (t == center) ? 1.0 : 0.0;
}

double Loudness::TpPhaseDcGain(int phase) const {
    if (phase < 0 || phase >= kOs) return 0.0;
    double s = 0.0;
    for (int t = 0; t < kTaps; ++t) s += fTpCoeffs[phase][t];
    return s;
}

// ---------------------------------------------------------------------------
// K-weighting filter design.
//
// BS.1770-4 specifies K-weighting as a high-shelf followed by a 2nd-order
// high-pass. The standard gives coefficients at 48 kHz; here we recompute them
// for the actual sample rate from the same analog prototypes using the
// pre-warped bilinear transform (frequency pre-warp K = tan(pi*fc/fs)), so the
// meter tracks the intended analog response at any rate. Prototype constants
// (fc, Q, gain) are the canonical BS.1770-4 values.
// ---------------------------------------------------------------------------
void Loudness::BuildKWeighting(double sr) {
    const double pi = 3.14159265358979323846;

    // Stage 1: high-shelf, ~+4 dB above ~1681 Hz.
    const double fcShelf = 1681.974450955533;
    const double gainDb  = 3.999843853973347;
    const double qShelf  = 0.7071752369554196;

    const double Ks = std::tan(pi * fcShelf / sr);
    const double Vh = std::pow(10.0, gainDb / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);

    const double a0s = 1.0 + Ks / qShelf + Ks * Ks;
    fShelf.b0 = (Vh + Vb * Ks / qShelf + Ks * Ks) / a0s;
    fShelf.b1 = 2.0 * (Ks * Ks - Vh) / a0s;
    fShelf.b2 = (Vh - Vb * Ks / qShelf + Ks * Ks) / a0s;
    fShelf.a1 = 2.0 * (Ks * Ks - 1.0) / a0s;
    fShelf.a2 = (1.0 - Ks / qShelf + Ks * Ks) / a0s;

    // Stage 2: 2nd-order high-pass, ~38 Hz.
    const double fcHp = 38.13547087602444;
    const double qHp  = 0.5003270373238773;

    const double Kh  = std::tan(pi * fcHp / sr);
    const double a0h = 1.0 + Kh / qHp + Kh * Kh;
    fHighPass.b0 = 1.0 / a0h;
    fHighPass.b1 = -2.0 / a0h;
    fHighPass.b2 = 1.0 / a0h;
    fHighPass.a1 = 2.0 * (Kh * Kh - 1.0) / a0h;
    fHighPass.a2 = (1.0 - Kh / qHp + Kh * Kh) / a0h;
}

// ---------------------------------------------------------------------------
void Loudness::Prepare(double sampleRate) {
    if (sampleRate > 0)
        fSampleRate = sampleRate;

    BuildKWeighting(fSampleRate);
    BuildTruePeak();

    const std::size_t momLen   = static_cast<std::size_t>(fSampleRate * 0.4);
    const std::size_t shortLen = static_cast<std::size_t>(fSampleRate * 3.0);
    fMomRing.assign(momLen > 0 ? momLen : 1, 0.0);
    fShortRing.assign(shortLen > 0 ? shortLen : 1, 0.0);

    fSubLen = static_cast<int>(fSampleRate * 0.1);  // 100 ms sub-block
    if (fSubLen < 1) fSubLen = 1;

    Reset();
}

void Loudness::Reset() {
    fShelf.ResetState();
    fHighPass.ResetState();

    std::fill(fMomRing.begin(), fMomRing.end(), 0.0);
    std::fill(fShortRing.begin(), fShortRing.end(), 0.0);
    fMomPos = fShortPos = 0;
    fMomSum = fShortSum = 0.0;

    fSubPos = 0;
    fSubAccum = 0.0;
    fSubHistory.clear();
    fBlockMeanSq.clear();

    for (int c = 0; c < 2; ++c) {
        for (int t = 0; t < kTaps; ++t)
            fTpHistory[c][t] = 0.0;
        fTpPos[c] = 0;
    }
    fMaxTruePeak = 0.0;
}

// ---------------------------------------------------------------------------
void Loudness::Process(const float* stereo, int frames) {
    if (!stereo || frames <= 0)
        return;
    // A default-constructed meter (no Prepare yet) has empty windows; measuring
    // is a no-op until Prepare() sizes the rings.
    if (fMomRing.empty() || fShortRing.empty())
        return;

    for (int i = 0; i < frames; ++i) {
        const double inL = stereo[i * 2 + 0];
        const double inR = stereo[i * 2 + 1];

        // --- True peak (before K-weighting; on the actual signal) ---
        for (int c = 0; c < 2; ++c) {
            const double s = (c == 0) ? inL : inR;
            fTpHistory[c][fTpPos[c]] = s;
            fTpPos[c] = (fTpPos[c] + 1) % kTaps;

            double localMax = std::fabs(s);
            for (int ph = 1; ph < kOs; ++ph) {
                double acc = 0.0;
                for (int t = 0; t < kTaps; ++t) {
                    const int idx = (fTpPos[c] - t - 1 + kTaps) % kTaps;
                    acc += fTpHistory[c][idx] * fTpCoeffs[ph][t];
                }
                localMax = std::max(localMax, std::fabs(acc));
            }
            fMaxTruePeak = std::max(fMaxTruePeak, localMax);
        }

        // --- K-weighting (high-shelf then high-pass) per channel ---
        const double kL = fHighPass.Tick(fShelf.Tick(inL, 0), 0);
        const double kR = fHighPass.Tick(fShelf.Tick(inR, 1), 1);

        // Summed-channel K-weighted power (stereo weights 1.0 each).
        const double power = kL * kL + kR * kR;

        // Momentary sliding window (running sum).
        fMomSum += power - fMomRing[fMomPos];
        fMomRing[fMomPos] = power;
        if (++fMomPos >= fMomRing.size()) fMomPos = 0;

        // Short-term sliding window (running sum).
        fShortSum += power - fShortRing[fShortPos];
        fShortRing[fShortPos] = power;
        if (++fShortPos >= fShortRing.size()) fShortPos = 0;

        // Integrated: accumulate 100 ms sub-blocks; every completed sub-block
        // forms the newest quarter of an overlapping 400 ms gating block.
        fSubAccum += power;
        if (++fSubPos >= fSubLen) {
            fSubHistory.push_back(fSubAccum);
            if (fSubHistory.size() > 4)
                fSubHistory.pop_front();

            // Once we have four 100 ms sub-blocks, a full 400 ms block exists.
            if (fSubHistory.size() == 4) {
                double blockSum = 0.0;
                for (double v : fSubHistory) blockSum += v;
                const double blockMeanSq =
                    blockSum / static_cast<double>(fSubLen * 4);
                fBlockMeanSq.push_back(blockMeanSq);
                // Cap history (~10 min at 100 ms hop) to bound memory.
                if (fBlockMeanSq.size() > 6000)
                    fBlockMeanSq.erase(fBlockMeanSq.begin());
            }

            fSubAccum = 0.0;
            fSubPos = 0;
        }
    }
}

// ---------------------------------------------------------------------------
float Loudness::MeanSquareToLufs(double meanSquare) {
    if (!(meanSquare > 1e-12))
        return kSilenceLufs;
    const double l = -0.691 + 10.0 * std::log10(meanSquare);
    return l < kSilenceLufs ? kSilenceLufs : static_cast<float>(l);
}

float Loudness::MomentaryLufs() const {
    if (fMomRing.empty()) return kSilenceLufs;
    return MeanSquareToLufs(fMomSum / static_cast<double>(fMomRing.size()));
}

float Loudness::ShortTermLufs() const {
    if (fShortRing.empty()) return kSilenceLufs;
    return MeanSquareToLufs(fShortSum / static_cast<double>(fShortRing.size()));
}

float Loudness::IntegratedLufs() const {
    if (fBlockMeanSq.empty())
        return kSilenceLufs;

    // Pass 1: absolute -70 LUFS gate; mean of surviving block mean squares.
    double sumAbs = 0.0;
    int    nAbs   = 0;
    for (double ms : fBlockMeanSq) {
        const double l = -0.691 + 10.0 * std::log10(ms > 1e-12 ? ms : 1e-12);
        if (l > kAbsoluteGateLufs) {
            sumAbs += ms;
            ++nAbs;
        }
    }
    if (nAbs == 0)
        return kSilenceLufs;

    const double meanAbs = sumAbs / static_cast<double>(nAbs);
    const double relThreshLufs =
        (-0.691 + 10.0 * std::log10(meanAbs)) + kRelativeGateLu;

    // Pass 2: relative gate (mean - 10 LU); mean of surviving block mean sq.
    double sumRel = 0.0;
    int    nRel   = 0;
    for (double ms : fBlockMeanSq) {
        const double l = -0.691 + 10.0 * std::log10(ms > 1e-12 ? ms : 1e-12);
        if (l > kAbsoluteGateLufs && l > relThreshLufs) {
            sumRel += ms;
            ++nRel;
        }
    }
    if (nRel == 0)
        return kSilenceLufs;

    return MeanSquareToLufs(sumRel / static_cast<double>(nRel));
}

float Loudness::TruePeakDb() const {
    if (!(fMaxTruePeak > 1e-12))
        return kSilenceDb;
    return static_cast<float>(20.0 * std::log10(fMaxTruePeak));
}

} // namespace daw
