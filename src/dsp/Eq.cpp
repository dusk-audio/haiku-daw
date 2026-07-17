#include "Eq.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Band type is fixed by position: low shelf, three peaks, high shelf.
enum BandType { LowShelf, Peak, HighShelf };
const BandType kType[Eq::kBands] = { LowShelf, Peak, Peak, Peak, HighShelf };

// Sensible default band frequencies / Qs (used before SetBand is called).
const float kDefFreq[Eq::kBands] = { 80.0f, 240.0f, 750.0f, 2200.0f, 6500.0f };
const float kDefQ[Eq::kBands]    = { 0.70f, 0.90f, 0.90f, 0.90f, 0.70f };

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
    // Shelf-slope radicand; with Q > 1 the (1/Q - 1) term is negative and can
    // drive this below zero at high gain -> sqrt(NaN). Clamp to keep it finite.
    double rad = (A + 1.0 / A) * (1.0 / Q - 1.0) + 2.0;
    if (rad < 0.0) rad = 0.0;
    const double alpha = sinW / 2.0 * std::sqrt(rad);
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
    // Shelf-slope radicand; with Q > 1 the (1/Q - 1) term is negative and can
    // drive this below zero at high gain -> sqrt(NaN). Clamp to keep it finite.
    double rad = (A + 1.0 / A) * (1.0 / Q - 1.0) + 2.0;
    if (rad < 0.0) rad = 0.0;
    const double alpha = sinW / 2.0 * std::sqrt(rad);
    const double b0 =  A * ((A + 1.0) + (A - 1.0) * cosW + 2.0 * sqA * alpha);
    const double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
    const double b2 =  A * ((A + 1.0) + (A - 1.0) * cosW - 2.0 * sqA * alpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cosW + 2.0 * sqA * alpha;
    const double a1 =  2.0 * ((A - 1.0) - (A + 1.0) * cosW);
    const double a2 = (A + 1.0) - (A - 1.0) * cosW - 2.0 * sqA * alpha;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

} // namespace

// ~10 ms coefficient glide: defeats zipper on a stepped band parameter without
// audibly smearing an automation move.
static constexpr double kCoefSmoothMs = 10.0;

Eq::Eq() {
    for (int b = 0; b < kBands; b++) {
        fFreq[b]   = kDefFreq[b];
        fGainDb[b] = 0.0f;
        fQ[b]      = kDefQ[b];
    }
    for (int b = 0; b < kBands; b++) ComputeBand(b);
    SnapCoeffs();
    Reset();
}

void Eq::SetBand(int band, float freqHz, float gainDb, float q) {
    if (band < 0 || band >= kBands) return;
    fFreq[band]   = freqHz;
    fGainDb[band] = gainDb;
    fQ[band]      = q;
    ComputeBand(band);
    if (fCoefSmooth <= 0.0)   // not yet Prepared: apply immediately (no glide)
        SnapCoeffs();
}

void Eq::Prepare(double sampleRate) {
    if (sampleRate > 0) fSampleRate = sampleRate;
    fCoefSmooth = 1.0 - std::exp(-1.0 / (kCoefSmoothMs * 0.001 * fSampleRate));
    for (int b = 0; b < kBands; b++) ComputeBand(b);
    SnapCoeffs();   // start on-target: no glide transient at playback start
    for (int i = 0; i < kFftSize; i++)                 // Hann window
        fWin[i] = 0.5f - 0.5f * (float)std::cos(2.0 * kPi * i / (kFftSize - 1));
    fCapPos = 0;
    fSnapIdx.store(-1, std::memory_order_relaxed);      // no published frame yet
    fSnapSeq.store(0, std::memory_order_relaxed);
    fMagCount = 0;
    fMagSeq   = 0;
    Reset();
}

// Iterative radix-2 Cooley-Tukey FFT in place (kit-free). n must be a power of 2.
static void Fft(double* re, double* im, int n) {
    for (int i = 1, j = 0; i < n; i++) {               // bit-reversal permutation
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            double cwr = 1.0, cwi = 0.0;
            for (int k = 0; k < len / 2; k++) {
                const int a = i + k, b = i + k + len / 2;
                const double tr = re[b] * cwr - im[b] * cwi;
                const double ti = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                const double ncwr = cwr * wr - cwi * wi;
                cwi = cwr * wi + cwi * wr; cwr = ncwr;
            }
        }
    }
}

// RT: buffer one mono input sample; when a full frame fills, publish it into
// the snapshot buffer the UI is NOT reading and bump the version. No FFT here —
// only a copy + two atomic stores, so the audio thread stays cheap.
void Eq::CaptureSpectrumSample(float mono) {
    fCap[fCapPos++] = mono;
    if (fCapPos < kFftSize) return;
    fCapPos = 0;
    const int pub = fSnapIdx.load(std::memory_order_relaxed);
    const int w   = (pub == 0) ? 1 : 0;   // write the buffer the UI isn't reading
    for (int i = 0; i < kFftSize; i++) fCapSnap[w][i] = fCap[i];
    fSnapIdx.store(w, std::memory_order_release);
    fSnapSeq.fetch_add(1, std::memory_order_release);
}

// UI thread: run the FFT on the newest published frame (only when a new one has
// arrived since last time), cache the magnitudes, and copy them out. The
// transform runs HERE, off the audio thread.
int Eq::Spectrum(float* magDb, int maxBins) const {
    const int idx = fSnapIdx.load(std::memory_order_acquire);
    if (idx >= 0) {
        const unsigned seq = fSnapSeq.load(std::memory_order_acquire);
        if (seq != fMagSeq) {   // a new frame was captured -> (re)transform
            for (int i = 0; i < kFftSize; i++) {
                fRe[i] = fCapSnap[idx][i] * fWin[i];
                fIm[i] = 0.0;
            }
            Fft(fRe, fIm, kFftSize);
            for (int k = 0; k < kBins; k++) {
                const double mag = std::sqrt(fRe[k] * fRe[k] + fIm[k] * fIm[k])
                                   / (kFftSize * 0.5);
                fMag[k] = (float)(20.0 * std::log10(mag > 1e-7 ? mag : 1e-7));
            }
            fMagCount = kBins;
            fMagSeq   = seq;
        }
    }
    const int cnt = fMagCount < maxBins ? fMagCount : maxBins;
    for (int i = 0; i < cnt; i++) magDb[i] = fMag[i];
    return cnt;
}

void Eq::ComputeBand(int b) {
    Co c;
    switch (kType[b]) {
        case LowShelf:  c = LowShelfCo(fFreq[b], fSampleRate, fGainDb[b], fQ[b]); break;
        case HighShelf: c = HighShelfCo(fFreq[b], fSampleRate, fGainDb[b], fQ[b]); break;
        case Peak:
        default:        c = Peaking(fFreq[b], fSampleRate, fGainDb[b], fQ[b]); break;
    }
    fTB0[b] = c.b0; fTB1[b] = c.b1; fTB2[b] = c.b2; fTA1[b] = c.a1; fTA2[b] = c.a2;
}

void Eq::SnapCoeffs() {
    for (int b = 0; b < kBands; b++) {
        fB0[b] = fTB0[b]; fB1[b] = fTB1[b]; fB2[b] = fTB2[b];
        fA1[b] = fTA1[b]; fA2[b] = fTA2[b];
    }
}

void Eq::Reset() {
    for (int b = 0; b < kBands; b++)
        for (int ch = 0; ch < 2; ch++) {
            fX1[b][ch] = fX2[b][ch] = 0.0;
            fY1[b][ch] = fY2[b][ch] = 0.0;
        }
}

void Eq::SetParam(int slot, float value) {
    if (slot < 0 || slot >= kBands * 3) return;
    const int b = slot / 3, which = slot % 3;
    float f = fFreq[b], g = fGainDb[b], q = fQ[b];
    if (which == 0) f = value; else if (which == 1) g = value; else q = value;
    SetBand(b, f, g, q);   // recomputes the band's coefficients (RT-safe)
}

float Eq::MagnitudeResponseDb(float freqHz) const {
    const double w = 2.0 * kPi * (double)freqHz / fSampleRate;
    const double c1 = std::cos(w),      s1 = std::sin(w);
    const double c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
    double totalDb = 0.0;
    for (int b = 0; b < kBands; b++) {
        // Draw the SETTLED (target) response so the curve tracks the knobs
        // immediately, not the gliding live coefficients.
        // H(e^jw) = (b0 + b1 e^-jw + b2 e^-2jw)/(1 + a1 e^-jw + a2 e^-2jw).
        const double nr = fTB0[b] + fTB1[b] * c1 + fTB2[b] * c2;
        const double ni = -(fTB1[b] * s1 + fTB2[b] * s2);
        const double dr = 1.0 + fTA1[b] * c1 + fTA2[b] * c2;
        const double di = -(fTA1[b] * s1 + fTA2[b] * s2);
        const double num = nr * nr + ni * ni;
        const double den = dr * dr + di * di;
        if (den > 1e-20 && num > 1e-20)
            totalDb += 10.0 * std::log10(num / den);   // 10*log10 of |H|^2
    }
    return (float)totalDb;
}

void Eq::Process(float* stereo, int frames) {
    for (int i = 0; i < frames; i++) {
        CaptureSpectrumSample(0.5f * (stereo[i * 2] + stereo[i * 2 + 1]));  // pre-EQ
        // Glide each band's live coefficients toward target (shared across
        // channels) so a stepped band parameter ramps in instead of clicking.
        for (int b = 0; b < kBands; b++) {
            fB0[b] += (fTB0[b] - fB0[b]) * fCoefSmooth;
            fB1[b] += (fTB1[b] - fB1[b]) * fCoefSmooth;
            fB2[b] += (fTB2[b] - fB2[b]) * fCoefSmooth;
            fA1[b] += (fTA1[b] - fA1[b]) * fCoefSmooth;
            fA2[b] += (fTA2[b] - fA2[b]) * fCoefSmooth;
        }
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
