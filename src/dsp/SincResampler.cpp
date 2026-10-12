#include "SincResampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daw {

namespace {

// Modified Bessel function of the first kind, order 0 — the Kaiser window's
// shape. The series converges quickly for the beta used here (a few dozen
// terms); the loop bound is a guard, not the usual exit.
double BesselI0(double x) {
    double sum  = 1.0;
    double term = 1.0;
    const double half = 0.5 * x;
    for (int k = 1; k < 64; k++) {
        term *= (half / k) * (half / k);
        sum  += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

} // namespace

SincResampler::SincResampler(double inRate, double outRate, int channels)
    : fChannels(channels < 1 ? 1
                             : (channels > kMaxChannels ? kMaxChannels : channels)) {
    // NaN or a non-positive rate would poison every step below, so stand in a
    // common one — a resampler built from a broken WAV header must still be
    // safe to call. (`!(x > 0)` is the NaN test.)
    if (!(inRate > 0.0))  inRate  = 48000.0;
    if (!(outRate > 0.0)) outRate = inRate;

    fRatio    = outRate / inRate;
    fStep     = 1.0 / fRatio;
    fIdentity = (inRate == outRate);

    Reset();
    if (!fIdentity) BuildKernel();
}

void SincResampler::Reset() {
    // The primed frames stand in for the source BEFORE its first sample, so
    // the first output frame's window reads background there instead of
    // running off the front of the buffer. They are filled with the stream's
    // own first sample on the next Process (see fNeedPrime).
    fBuf.assign((size_t)(kPrime + kTaps) * (size_t)fChannels, 0.0f);
    fValid     = (size_t)kPrime;
    fPos       = (double)kPrime;
    fNeedPrime = true;
}

void SincResampler::Emit(std::vector<float>& out) {
    const size_t ch   = (size_t)fChannels;
    const long   fl   = (long)std::floor(fPos);
    const long   base = fl - (kTaps / 2 - 1);
    // Truncated, never rounded: rounding a frac just under 1 up to the next
    // integer would leave the tap INDICES on this sample while the weights
    // describe the next one — a full sample of misalignment.
    const int    phase = (int)((fPos - (double)fl) * (double)kPhases);  // [0,kPhases)
    const float* row   = fTable.data() + (size_t)phase * (size_t)kTaps;
    const float* bp    = fBuf.data() + (size_t)base * ch;

    for (size_t c = 0; c < ch; c++) {
        float acc = 0.0f;
        for (int t = 0; t < kTaps; t++)
            acc += row[t] * bp[(size_t)t * ch + c];
        out.push_back(acc);
    }
}

void SincResampler::BuildKernel() {
    // Band-limit to the LOWER of the two Nyquists, less half the Kaiser
    // window's transition width: the stopband edge then lands exactly ON that
    // Nyquist (nothing above it survives to fold back) and the passband edge a
    // whole transition band below it.
    //
    // The transition width comes from the Kaiser design formulas —
    // attenuation A = 8.7 + beta / 0.1102 dB and N = (A - 8) / (2.285 * dw) —
    // so the window's own parameters decide where the band edges go instead of
    // a hand-tuned constant drifting away from them.
    const double attenDb  = 8.7 + kKaiserBeta / 0.1102;
    const double dwRad    = (attenDb - 8.0) / (2.285 * (double)(kTaps - 1));
    const double dwCycles = dwRad / (2.0 * M_PI);
    double fc = 0.5 * std::min(1.0, fRatio) - 0.5 * dwCycles;
    if (!(fc > 1e-4)) fc = 1e-4;   // degenerate ratio: keep a usable kernel

    fTable.assign((size_t)kPhases * (size_t)kTaps, 0.0f);

    const double denom  = BesselI0(kKaiserBeta);
    const double H      = 0.5 * (double)kTaps;   // window half-width, input frames
    const double centre = (double)(kTaps / 2 - 1);

    for (int ph = 0; ph < kPhases; ph++) {
        const double frac = (double)ph / (double)kPhases;
        float* row = fTable.data() + (size_t)ph * (size_t)kTaps;
        double sum = 0.0;
        for (int t = 0; t < kTaps; t++) {
            // Distance, in input frames, between the interpolation point and
            // this tap's sample. Zero lands on the centre tap.
            const double u = frac + centre - (double)t;

            double w = 0.0;
            if (std::fabs(u) < H) {           // Kaiser, zero at the support edge
                const double r = u / H;
                w = BesselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / denom;
            }
            const double s = (std::fabs(u) < 1e-12)
                                 ? 2.0 * fc
                                 : std::sin(2.0 * M_PI * fc * u) / (M_PI * u);
            const double v = s * w;
            row[t] = (float)v;
            sum   += v;
        }
        // Unity DC gain per PHASE, not merely on average: a constant signal
        // must come back as exactly that constant, whatever the phase.
        if (sum != 0.0)
            for (int t = 0; t < kTaps; t++)
                row[t] = (float)((double)row[t] / sum);
    }
}

void SincResampler::Process(const float* in, size_t inFrames,
                            std::vector<float>& out) {
    if (!in || inFrames == 0) return;

    const size_t ch = (size_t)fChannels;

    if (fIdentity) {   // bit-exact passthrough; see the header
        out.insert(out.end(), in, in + inFrames * ch);
        return;
    }

    // Append the new input after the history the previous call kept, and zero
    // the kTaps frames of slack after it. The window of an output near the end
    // of the stream reads past the data; that must be silence, not whatever
    // the last call left there.
    fBuf.resize((fValid + inFrames + (size_t)kTaps) * ch, 0.0f);
    std::memcpy(fBuf.data() + fValid * ch, in, inFrames * ch * sizeof(float));
    const size_t total = fValid + inFrames;
    std::memset(fBuf.data() + total * ch, 0, (size_t)kTaps * ch * sizeof(float));

    if (fNeedPrime) {
        // The frames before the stream now hold the stream's own first sample
        // rather than silence, so a clip that starts on a transient starts at
        // its own level. Done here, once, so it cannot depend on chunk sizes.
        for (size_t i = 0; i < (size_t)kPrime; i++)
            for (size_t c = 0; c < ch; c++)
                fBuf[i * ch + c] = fBuf[(size_t)kPrime * ch + c];
        fNeedPrime = false;
    }

    // One output frame per position, but ONLY while its whole filter window is
    // real data: a window reaching into samples the caller has not handed over
    // yet would make the output depend on the chunk sizes, and the decode loop
    // does not control those. The tail therefore stays pending until more
    // input arrives, or Flush() declares the end of the stream.
    while ((double)((long)std::floor(fPos) + (kTaps / 2)) < (double)total) {
        Emit(out);
        fPos += fStep;
    }

    // Keep only what the next window still needs — at most kTaps frames — and
    // move it to the front so the buffer does not grow across calls.
    const long nextBase = (long)std::floor(fPos) - (kTaps / 2 - 1);
    size_t keepStart = nextBase > 0 ? (size_t)nextBase : 0;
    if (keepStart > total) keepStart = total;
    const size_t kept = total - keepStart;
    if (keepStart > 0)
        std::memmove(fBuf.data(), fBuf.data() + keepStart * ch,
                     kept * ch * sizeof(float));
    fPos  -= (double)keepStart;
    fValid = kept;
    // A flushed output frame reads up to kTaps frames past the data, and the
    // shift just moved real samples into part of that region; put the silence
    // back so the future a Flush() sees is the future, not the last chunk.
    std::memset(fBuf.data() + fValid * ch, 0, (size_t)kTaps * ch * sizeof(float));
}

void SincResampler::Flush(std::vector<float>& out) {
    if (fIdentity) return;   // a passthrough carries nothing
    // The stream is over: positions whose window reaches past the last real
    // sample are built now, with the future read as silence (the slack the
    // buffer already keeps zeroed).
    while (fPos < (double)fValid) {
        Emit(out);
        fPos += fStep;
    }
}

} // namespace daw
