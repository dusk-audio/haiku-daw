#include "Stretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace daw {

namespace {

// The algorithm's constants, in seconds (the header documents what they do).
constexpr double kWindowSeconds = 0.050;   // L  = 50 ms
constexpr double kSearchSeconds = 0.010;   // R  = 10 ms

// The cross-correlation is evaluated on every 4th sample of the overlap: the
// peak's SHAPE is what matters and a quarter of the multiplies costs a quarter
// of the time. The lag itself is then refined at full resolution around the
// coarse winner, so a correlation of a decimated signal cannot pick a lag a
// few samples off.
constexpr int kCoarseLagStep = 4;    // candidate offsets per coarse pass
constexpr int kCorrelationStep = 4;  // overlap samples per correlation term

} // namespace

void Stretcher::Prepare(double sampleRate, int channels, double ratio) {
    fChannels = channels < 1 ? 1 : (channels > kMaxChannels ? kMaxChannels : channels);
    fRatio    = (ratio > 0.0) ? ratio : 1.0;
    fIdentity = std::fabs(fRatio - 1.0) < 1e-12;

    if (!(sampleRate > 0.0)) sampleRate = 48000.0;
    fWindow = (int)std::llround(kWindowSeconds * sampleRate);
    if (fWindow < 16) fWindow = 16;
    fWindow &= ~1;                       // even: the hop is exactly half of it
    fHop     = fWindow / 2;              // Hs = L/2 -> the windows sum to 1
    fSearch  = (int)std::llround(kSearchSeconds * sampleRate);
    if (fSearch < 1) fSearch = 1;
    fOverlap = fWindow - fHop;

    // Periodic Hann: w[i] = 0.5 - 0.5*cos(2*pi*i/L). Periodic (not the
    // symmetric (L-1) variant) because that is the one two windows a hop apart
    // sum to exactly 1 with — the overlap-add is unity gain by construction.
    fWin.resize((size_t)fWindow);
    for (int i = 0; i < fWindow; i++)
        fWin[(size_t)i] = 0.5f - 0.5f
            * (float)std::cos(2.0 * M_PI * (double)i / (double)fWindow);

    // History capacity: enough for the widest span a single frame's search
    // needs. A frame reads from min(Q(k) - R, Q(k-1) + Hs) — the target of the
    // similarity search sits BEHIND the read head when the ratio is below 1,
    // by up to Ha - Hs = Hs (1/ratio - 1) at the model's 0.25 floor (3*Hs) —
    // through Q(k) + R + L. Plus one extra hop of slack for chunked pushes.
    const int span = fWindow + fSearch + 3 * fHop + 2 * fHop + 256;
    fHistCap = span;
    fHist.resize((size_t)fHistCap * fChannels);
    fTail.resize((size_t)fHop * fChannels);
    fHopBuf.resize((size_t)fHop * fChannels);

    fPrepared = true;
    Reset(0);
}

void Stretcher::Reset(int64_t sourcePos) {
    if (sourcePos < 0) sourcePos = 0;
    fHistBase = sourcePos;
    fCount    = 0;
    fInputEnd = sourcePos;
    fPushed   = 0;
    fFlushed  = false;
    fDone     = false;
    fReadHi   = sourcePos;

    // Back onto the fixed grid: the frame that covers the requested source
    // position, resuming at the output frame that position maps to. The stream
    // origin (source 0 == output 0) is the grid's anchor, so this is the same
    // phase continuous playback would have had there — the only thing missing
    // is the previous frame's tail, which reads as the ramp-in (see the header
    // for the WarmupFrames() recipe that avoids even that).
    fNextOut  = (int64_t)std::llround((double)sourcePos * fRatio);
    fBuildIdx = fNextOut / fHop;
    fPendingSkip = (int)(fNextOut - fBuildIdx * fHop);
    fWarm     = (fBuildIdx > 0);   // frame fBuildIdx-1 exists -> its tail is the truth
    fHopFilled = fHopEmitted = 0;
    std::fill(fTail.begin(), fTail.end(), 0.0f);
    // The frame before the first one built has no predecessor of its own, so
    // the first frame's similarity target is the grid's own continuation —
    // the best guess a reset can make, and the exact answer for a run that
    // starts at the origin.
    fPrevRead = GridPos(fBuildIdx - 1);
}

Stretcher::Result Stretcher::Process(const float* in, int inFrames,
                                     float* out, int outCap) {
    Result r;
    if (fIdentity) {
        // Exact copy: the ratio-1 clip that still routes through the class
        // (the engine and the exporter both skip it entirely) must not gain a
        // window's worth of anything.
        if (!in || !out || inFrames <= 0 || outCap <= 0) return r;
        const int n = std::min(inFrames, outCap);
        std::memcpy(out, in, (size_t)n * fChannels * sizeof(float));
        r.inConsumed = r.outProduced = n;
        fNextOut += n;
        fInputEnd += n;
        fPushed   += n;
        fReadHi    = std::max(fReadHi, fInputEnd);
        return r;
    }
    if (!fPrepared) return r;

    // 1. Take input. The history is bounded, so a caller with a big chunk
    //    pushes the rest next call (the consumed count says how much landed).
    if (in && inFrames > 0) {
        Prune();
        const int room = fHistCap - (int)fCount;
        const int take = std::min(inFrames, room);
        if (take > 0) {
            std::memcpy(fHist.data() + fCount * fChannels, in,
                        (size_t)take * fChannels * sizeof(float));
            fCount    += (size_t)take;
            fInputEnd += take;
            fPushed   += take;
            r.inConsumed = take;
        }
    }

    // 2. Produce.
    if (out && outCap > 0)
        r.outProduced = Drain(out, outCap);
    return r;
}

void Stretcher::Flush() { fFlushed = true; }

int64_t Stretcher::WarmupFrames() const {
    // Start far enough before the target that the frame BEFORE the one holding
    // it can be read without clamping: its read start is Q(k-1) - R at worst,
    // and Q(k-1) is one analysis hop (Hs / ratio) before Q(k).
    if (fIdentity) return 0;
    return fHop + (int64_t)std::ceil((double)(fSearch + 2) * fRatio) + 2;
}

int64_t Stretcher::GridPos(int64_t k) const {
    return (int64_t)std::llround((double)k * (double)fHop / fRatio);
}

bool Stretcher::FrameReady(int64_t k) const {
    if (!fPrepared) return false;
    if (fFlushed) return true;   // past the end reads as silence
    // The frame's whole search window has to be in the history before its
    // offset may be chosen: a decision made on half a window is a decision the
    // caller's chunking would change.
    const int64_t q = GridPos(k);
    return fInputEnd >= q + fSearch + fWindow;
}

float Stretcher::Sample(int c, int64_t at) const {
    const int64_t i = at - fHistBase;
    if (i < 0 || i >= (int64_t)fCount) return 0.0f;
    return fHist[(size_t)i * fChannels + c];
}

void Stretcher::Prune() {
    if (fWarm) return;   // the warm-up frame may read further back (see BuildNext)
    // What the next frame built will read: its candidate window's low end and
    // its similarity target (the last frame's read, one hop on), whichever is
    // further back.
    const int64_t q = GridPos(fBuildIdx);
    int64_t floor = q - fSearch;
    if (fBuildIdx > 0)
        floor = std::min(floor, fPrevRead + fHop);
    floor -= 1;
    if (floor <= fHistBase) return;
    int64_t drop = floor - fHistBase;
    if (drop > (int64_t)fCount) drop = (int64_t)fCount;
    if (drop <= 0) return;
    std::memmove(fHist.data(), fHist.data() + (size_t)drop * fChannels,
                 (fCount - (size_t)drop) * fChannels * sizeof(float));
    fCount    -= (size_t)drop;
    fHistBase += drop;
}

int Stretcher::ChooseDelta(int64_t k) {
    if (k <= 0) return 0;   // frame 0: the grid itself is the only anchor
    const int64_t q = GridPos(k);
    // The target is the PREVIOUS FRAME'S OWN samples, one synthesis hop on:
    // what it actually placed in the output overlap, so the crossfade between
    // its falling half and this frame's rising half is coherent.
    const int64_t t = fPrevRead + fHop;

    // Is enough of the target in hand to judge anything? (A target that is
    // mostly outside the pushed input would make every candidate look alike,
    // and the winner would be an artefact of where the caller started.)
    int have = 0, want = 0;
    for (int i = 0; i < fOverlap; i += kCorrelationStep) {
        want++;
        const int64_t at = t + i;
        if (at >= fHistBase && at < fHistBase + (int64_t)fCount) have++;
    }
    if (have * 2 < want) return 0;

    auto mono = [&](int64_t at) -> float {
        if (fChannels == 1) return Sample(0, at);
        return (Sample(0, at) + Sample(1, at)) * 0.5f;
    };

    double tEnergy = 0.0;
    for (int i = 0; i < fOverlap; i += kCorrelationStep) {
        const double v = (double)mono(t + i);
        tEnergy += v * v;
    }
    if (tEnergy < 1e-12) return 0;   // the target is silence: nothing to match

    // Normalised cross-correlation of the candidate against the target. Ties go
    // to the nominal position (the tiny |d| penalty), and d is scanned from the
    // centre outward so a flat correlation cannot pick a lopsided winner by
    // accident of iteration order.
    auto score = [&](int d) -> double {
        const int64_t a = q + d;
        double st = 0.0, sa = 0.0, sta = 0.0;
        for (int i = 0; i < fOverlap; i += kCorrelationStep) {
            const double cv = (double)mono(a + i);
            const double tv = (double)mono(t + i);
            sta += cv * tv; sa += cv * cv; st += tv * tv;
        }
        const double den = std::sqrt(sa * st);
        return den > 1e-12 ? sta / den : -2.0;
    };

    int best = 0;
    double bestScore = -2.0;
    for (int d = 0; d <= fSearch; d += kCoarseLagStep) {
        for (int s = (d == 0) ? 1 : 2; s-- > 0;) {   // 0, -4, +4, -8, +8, ...
            const int dd = (s == 0) ? -d : d;
            if (dd < -fSearch || dd > fSearch) continue;
            const double v = score(dd) - 1e-6 * std::abs(dd);
            if (v > bestScore) { bestScore = v; best = dd; }
        }
    }
    // Refine at full lag resolution around the coarse winner.
    for (int d = best - (kCoarseLagStep - 1); d <= best + (kCoarseLagStep - 1); d++) {
        if (d < -fSearch || d > fSearch) continue;
        const double v = score(d) - 1e-6 * std::abs(d);
        if (v > bestScore) { bestScore = v; best = d; }
    }
    return best;
}

void Stretcher::BuildFrame(int64_t k, bool skipAll) {
    const int64_t a = GridPos(k) + ChooseDelta(k);
    const int ch = fChannels;

    // The hop's output: what the previous frame's tail already put there, plus
    // this frame's rising half.
    for (int i = 0; i < fHop; i++) {
        for (int c = 0; c < ch; c++) {
            const float w = fWin[(size_t)i];
            fHopBuf[(size_t)i * ch + c] =
                fTail[(size_t)i * ch + c] + w * Sample(c, a + i);
        }
    }
    // The new tail: this frame's falling half, which the NEXT hop adds on top.
    for (int i = 0; i < fHop; i++) {
        const float w = fWin[(size_t)(fHop + i)];
        for (int c = 0; c < ch; c++)
            fTail[(size_t)i * ch + c] = w * Sample(c, a + fHop + i);
    }
    if (a + fWindow > fReadHi) fReadHi = a + fWindow;
    fPrevRead = a;   // the next frame aligns its crossfade against this

    fHopFilled  = fHop;
    fHopEmitted = skipAll ? fHop : fPendingSkip;
    if (!skipAll) fPendingSkip = 0;
}

bool Stretcher::BuildNext() {
    if (fWarm) {
        // One frame before the resume point, for its tail alone: the frame that
        // continuous playback would have synthesized there. Discarded (its hop
        // is before the requested output index), but it makes fTail the real
        // OLA history instead of the ramp-in Reset() would otherwise give.
        if (fBuildIdx == 0) {
            fWarm = false;
        } else {
            if (!FrameReady(fBuildIdx - 1)) return false;   // wait for the input
            fWarm = false;
            BuildFrame(fBuildIdx - 1, /*skipAll=*/true);
            return true;
        }
    }
    if (fPushed == 0) {   // nothing was ever pushed: nothing to produce
        fDone = true;
        return false;
    }
    if (fFlushed && GridPos(fBuildIdx) >= fInputEnd) {
        // The material is spent. The pending tail is the stream's last hop: the
        // output ends within one synthesis hop of in * ratio, as the spec's
        // length check wants, and the frames whose windows run past the end
        // fade out rather than continuing into silence-read-as-silence.
        if (fDone) return false;
        std::copy(fTail.begin(), fTail.end(), fHopBuf.begin());
        fHopFilled  = fHop;
        fHopEmitted = 0;
        fDone = true;
        return true;
    }
    if (!FrameReady(fBuildIdx)) return false;   // needs more input
    BuildFrame(fBuildIdx, /*skipAll=*/false);
    fBuildIdx++;
    return true;
}

int Stretcher::Drain(float* out, int cap) {
    const int ch = fChannels;
    int n = 0;
    while (n < cap) {
        if (fHopEmitted < fHopFilled) {
            const int k = std::min(fHopFilled - fHopEmitted, cap - n);
            std::memcpy(out + (size_t)n * ch,
                        fHopBuf.data() + (size_t)fHopEmitted * ch,
                        (size_t)k * ch * sizeof(float));
            fHopEmitted += k;
            n           += k;
            fNextOut    += k;
            continue;
        }
        if (fDone) break;
        if (!BuildNext()) break;
    }
    return n;
}

} // namespace daw
