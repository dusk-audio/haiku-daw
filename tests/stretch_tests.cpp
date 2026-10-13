// Host-buildable tests for the WSOLA time-stretcher (M7.2) and the model-side
// stretch policy it serves.
//
// What is asserted here is the whole contract the engine and the exporter lean
// on: the output length is in * ratio, a sine keeps its frequency (a pitch
// vocoder would shift it, a resampler would too), the result does not depend on
// how the caller chunks its pushes and pulls, a run that resumes mid-clip
// reproduces continuous playback from one window past the resume (the fixed
// synthesis grid), and nothing in the output clicks or dips at a seam.

#include "../src/dsp/Stretch.h"
#include "../src/model/ClipStretch.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// --- helpers -----------------------------------------------------------

// Drive the push model to exhaustion: push `in[from...]` in `chunkIn`-frame
// pieces, pull `chunkOut`-frame pieces, and flush once everything is in. The
// point is that the RESULT must not depend on either chunk size.
static std::vector<float> Render(Stretcher& st, const std::vector<float>& in,
                                 size_t from, int chunkIn, int chunkOut) {
    std::vector<float> out;
    std::vector<float> buf((size_t)chunkOut);
    size_t pos = from;
    bool flushed = false;
    // A runaway guard, not a tolerance: a stretcher that never stops producing
    // (the mutation that removes its end rule) must terminate the test, and the
    // length assertions below then fail the way they should.
    const size_t cap = in.size() * 4 + 100000;
    for (;;) {
        const size_t want = (pos < in.size())
            ? std::min<size_t>((size_t)chunkIn, in.size() - pos) : 0;
        const float* p = want > 0 ? in.data() + pos : nullptr;
        Stretcher::Result r = st.Process(p, (int)want, buf.data(), chunkOut);
        pos += (size_t)r.inConsumed;
        if (r.outProduced > 0)
            out.insert(out.end(), buf.begin(), buf.begin() + r.outProduced);
        if (pos >= in.size() && !flushed) { st.Flush(); flushed = true; }
        if (flushed && r.inConsumed == 0 && r.outProduced == 0) break;
        if (out.size() > cap) break;
    }
    return out;
}

static std::vector<float> Sine(double rate, double hz, double seconds,
                               float amp = 0.5f) {
    const size_t n = (size_t)(rate * seconds);
    std::vector<float> v(n);
    for (size_t i = 0; i < n; i++)
        v[i] = amp * (float)std::sin(2.0 * M_PI * hz * (double)i / rate);
    return v;
}

// Rising zero crossings of a signal over [from, to) — the frequency probe.
static int ZeroCrossings(const std::vector<float>& v, size_t from, size_t to) {
    int n = 0;
    if (to > v.size()) to = v.size();
    for (size_t i = from + 1; i < to; i++)
        if (v[i - 1] <= 0.0f && v[i] > 0.0f) n++;
    return n;
}

static float MaxStep(const std::vector<float>& v, size_t from, size_t to) {
    float m = 0.0f;
    if (to > v.size()) to = v.size();
    for (size_t i = from + 1; i < to; i++)
        m = std::max(m, std::fabs(v[i] - v[i - 1]));
    return m;
}

// RMS of each `win`-frame window: used to prove the overlap-add neither dips
// nor doubles at a seam.
static void WindowRms(const std::vector<float>& v, size_t win, size_t from,
                      size_t to, float* lo, float* hi) {
    *lo = 1e9f; *hi = 0.0f;
    if (to > v.size()) to = v.size();
    for (size_t s = from; s + win <= to; s += win / 2) {
        double acc = 0.0;
        for (size_t i = s; i < s + win; i++) acc += (double)v[i] * v[i];
        const float r = (float)std::sqrt(acc / (double)win);
        *lo = std::min(*lo, r); *hi = std::max(*hi, r);
    }
}

int main() {
    const double SR = 48000.0;

    // --- 1. output length, and the ratio-1 bypass -----------------------
    {
        const double ratios[] = { 0.5, 0.8, 1.0, 1.25, 2.0 };
        for (double r : ratios) {
            Stretcher st;
            st.Prepare(SR, 1, r);
            CHECK(st.HopFrames() > 0 && st.WindowFrames() == 2 * st.HopFrames());
            const std::vector<float> in = Sine(SR, 440.0, 1.0);
            std::vector<float> out = Render(st, in, 0, 8192, 4096);
            const double want = (double)in.size() * r;
            const double got  = (double)out.size();
            std::printf("    ratio %.2f: %zu in -> %zu out (want %.0f)\n",
                        r, in.size(), out.size(), want);
            CHECK(std::fabs(got - want) <= 1.5 * (double)st.HopFrames());
            if (r == 1.0) {
                // Bit-exact passthrough: the ratio-1 clip takes the copy path
                // even when it is routed through the class, and neither engine
                // nor exporter routes it through at all.
                CHECK(st.IsIdentity());
                CHECK(out.size() == in.size());
                bool same = out.size() == in.size();
                for (size_t i = 0; same && i < out.size(); i++)
                    if (out[i] != in[i]) same = false;
                CHECK(same);
            } else {
                CHECK(!st.IsIdentity());
                if (r > 1.0) CHECK(out.size() > in.size());
                else         CHECK(out.size() < in.size());
            }
        }
    }

    // --- 2. pitch is preserved: a sine stays at its frequency -----------
    {
        // Zero crossings over the steady middle: the head and tail ramps are
        // excluded so their extra crossings cannot flatter or spoil the count.
        const double ratios[] = { 0.5, 0.8, 1.25, 2.0 };
        for (double r : ratios) {
            Stretcher st;
            st.Prepare(SR, 1, r);
            // Long enough that one miscounted crossing at ratio 0.5 is a
            // rounding, not the tolerance: the count has to resolve cents.
            const std::vector<float> in = Sine(SR, 440.0, 6.0);
            const std::vector<float> out = Render(st, in, 0, 8192, 4096);
            const size_t skip = (size_t)(0.2 * SR);
            const size_t from = skip, to = out.size() > skip ? out.size() - skip : 0;
            CHECK(to > from);
            const double seconds = (double)(to - from) / SR;
            const double hz = ZeroCrossings(out, from, to) / seconds;
            const double cents = 1200.0 * std::log2(hz / 440.0);
            std::printf("    ratio %.2f: %.2f Hz (%.1f cents)\n", r, hz, cents);
            CHECK(std::fabs(cents) < 10.0);
        }
    }

    // --- 3. determinism -------------------------------------------------
    {
        const std::vector<float> in = Sine(SR, 330.0, 1.0);
        Stretcher a; a.Prepare(SR, 1, 1.37);
        Stretcher b; b.Prepare(SR, 1, 1.37);
        Stretcher c; c.Prepare(SR, 1, 1.37);
        // Chunked pushes/pulls must not change a single sample: the search only
        // decides once its whole window has arrived.
        const std::vector<float> ra = Render(a, in, 0, 100000, 100000);
        const std::vector<float> rb = Render(b, in, 0, 97, 61);
        const std::vector<float> rc = Render(c, in, 0, 3000, 512);
        CHECK(ra.size() == rb.size() && ra.size() == rc.size());
        bool same = ra.size() == rb.size() && ra.size() == rc.size();
        for (size_t i = 0; same && i < ra.size(); i++)
            if (ra[i] != rb[i] || ra[i] != rc[i]) same = false;
        CHECK(same);

        // A run resumed mid-clip lands on the SAME GRID: the output index it
        // resumes at is exactly round(sourcePos * ratio) — the frame phase
        // continuous playback had there, not a restarted one — and the material
        // it plays from there is continuous playback's material to within the
        // search radius. (The similarity correction is chained to what the
        // previous frame actually read, which is what keeps the crossfades
        // coherent; a resumed run re-seeds that chain from the grid, so its
        // segment choices can differ from a continuous run's by up to R — see
        // the header. What must NOT differ is the timeline: the frames land
        // where they belong, so the material is the same audio, not a shifted
        // or drifted copy of it.)
        const int64_t midSrc = 20000;
        Stretcher d; d.Prepare(SR, 1, 1.37);
        const int64_t oMid = (int64_t)std::llround((double)midSrc * 1.37);
        const int64_t oStart = oMid - d.WarmupFrames();
        CHECK(oStart >= 0);
        const int64_t pReset = (int64_t)std::llround((double)oStart / 1.37);
        d.Reset(pReset);
        CHECK(d.OutputIndex() == (int64_t)std::llround((double)pReset * 1.37));
        const int64_t oActual = d.OutputIndex();
        const std::vector<float> rd = Render(d, in, (size_t)pReset, 4096, 2048);
        const size_t drop = (size_t)(oMid - oActual);
        CHECK(drop < rd.size());
        // The resumed material matches continuous playback's at some lag within
        // the search radius (the two runs' corrections differ, but every frame
        // reads within R of its nominal in both).
        {
            const size_t n = std::min<size_t>(20000, std::min(ra.size() - (size_t)oMid,
                                                              rd.size() - drop));
            CHECK(n > 10000);
            const int R = d.SearchFrames();
            double best = -2.0; int bestLag = 0;
            for (int lag = -2 * R; lag <= 2 * R; lag += 4) {
                double sa = 0, sb = 0, sab = 0;
                for (size_t i = 0; i < n; i += 7) {
                    const int64_t ja = (int64_t)((size_t)oMid + i) + lag;
                    if (ja < 0 || (size_t)ja >= ra.size()) continue;
                    const double a = ra[(size_t)ja], b = rd[drop + i];
                    sa += a * a; sb += b * b; sab += a * b;
                }
                const double den = std::sqrt(sa * sb);
                if (den > 0 && sab / den > best) { best = sab / den; bestLag = lag; }
            }
            std::printf("    resume lag %d (R=%d), correlation %.3f\n",
                        bestLag, R, best);
            CHECK(std::abs(bestLag) <= 2 * R);
            CHECK(best > 0.9);
        }
    }

    // --- 4. no clicks, no holes -----------------------------------------
    {
        Stretcher st;
        st.Prepare(SR, 1, 0.8);
        const std::vector<float> in = Sine(SR, 220.0, 1.0);
        const std::vector<float> out = Render(st, in, 0, 4096, 4096);
        // The window overlap is unity gain, so a steady tone keeps its level
        // across every seam...
        float lo = 0.0f, hi = 0.0f;
        WindowRms(out, 480, (size_t)st.WindowFrames(),
                  out.size() - (size_t)st.WindowFrames(), &lo, &hi);
        const float inRms = 0.5f / (float)std::sqrt(2.0);   // the sine's own RMS
        std::printf("    window RMS %.4f..%.4f (source %.4f)\n", lo, hi, inRms);
        CHECK(lo > inRms * 0.8f && hi < inRms * 1.2f);
        // ...and a click would show up as a step far beyond the source's own.
        const float srcStep = MaxStep(in, 0, in.size());
        const float outStep = MaxStep(out, (size_t)st.WindowFrames(),
                                      out.size() - (size_t)st.WindowFrames());
        std::printf("    max step %.4f (source %.4f)\n", outStep, srcStep);
        CHECK(outStep < srcStep * 2.0f);
    }

    // --- 5. seek quantisation -------------------------------------------
    {
        // Reset() puts the stream back on the clip-anchored grid at the output
        // frame the source position maps to, for any ratio — that is the whole
        // point of quantising the phase instead of restarting it.
        const double ratios[] = { 0.5, 0.8, 1.25, 2.0 };
        for (double r : ratios) {
            Stretcher st;
            st.Prepare(SR, 1, r);
            const int64_t pos[] = { 0, 1, 5000, 48000, 12345 };
            for (int64_t p : pos) {
                st.Reset(p);
                CHECK(st.OutputIndex() == (int64_t)std::llround((double)p * r));
            }
        }
        // Two independent instances, same input, same reset: identical output.
        Stretcher a; a.Prepare(SR, 1, 1.9);
        Stretcher b; b.Prepare(SR, 1, 1.9);
        a.Reset(777); b.Reset(777);
        const std::vector<float> in = Sine(SR, 300.0, 0.5);
        const std::vector<float> ra = Render(a, in, 777, 512, 384);
        const std::vector<float> rb = Render(b, in, 777, 2048, 1024);
        CHECK(ra.size() == rb.size());
        bool same = ra.size() == rb.size();
        for (size_t i = 0; same && i < ra.size(); i++)
            if (ra[i] != rb[i]) same = false;
        CHECK(same);
    }

    // --- 6. the model-side policy: the drag derives the ratio from the
    //        clip's TRIMMED span, not from the file ----------------------
    {
        Clip c;
        c.startFrame   = 0;
        c.lengthFrames = 24000;     // 0.5 s of timeline at 48 k
        c.sourceOffset = 4800;      // trimmed 0.1 s into the file
        c.stretchRatio = 1.0;
        const Frame fileFrames = 48000;   // 1 s file: the span is HALF of it

        // Stretching to 1.5x the timeline length: the ratio is 1.5 (the span it
        // references is unchanged) — NOT 1.5 * (fileFrames / span) = 3.0, which
        // is what dividing by the file length would give.
        const double r = StretchRatioForDrag(c, 36000, fileFrames, 48000.0, 48000.0);
        CHECK(std::fabs(r - 1.5) < 1e-9);
        // Halving the timeline length halves the ratio.
        CHECK(std::fabs(StretchRatioForDrag(c, 12000, fileFrames, 48000.0, 48000.0)
                        - 0.5) < 1e-9);
        // The span follows the OLD ratio: a clip already stretched to 2.0 has
        // half as much source per timeline frame, so the same 1.5x drag lands
        // on 3.0.
        c.stretchRatio = 2.0;
        CHECK(std::fabs(StretchRatioForDrag(c, 36000, fileFrames, 48000.0, 48000.0)
                        - 3.0) < 1e-9);
        c.stretchRatio = 1.0;

        // A source rate other than the project rate does NOT change the ratio:
        // the derivation is in timeline units, and the span a clip references is
        // the same span whatever rate the file happens to be at.
        CHECK(std::fabs(StretchRatioForDrag(c, 24000, 96000, 96000.0, 48000.0)
                        - 1.0) < 1e-9);
        CHECK(std::fabs(StretchRatioForDrag(c, 12000, 96000, 96000.0, 48000.0)
                        - 0.5) < 1e-9);

        // The file's hard end clamps the span: a clip whose recorded span runs
        // past what the file holds derives its ratio from the material that
        // exists, and a span entirely past the end still produces a finite
        // ratio instead of a division by nothing.
        Clip d = c;
        d.sourceOffset = 47000;     // only 1000 frames left in the file
        const double re = StretchRatioForDrag(d, 24000, fileFrames, 48000.0, 48000.0);
        CHECK(re > 0.0 && re <= kMaxStretchRatio);
        CHECK(std::fabs(re - kMaxStretchRatio) < 1e-9);   // 24000 / 1000 clamps
        Clip e = c;
        e.sourceOffset = fileFrames;   // nothing left at all
        const double rz = StretchRatioForDrag(e, 24000, fileFrames, 48000.0, 48000.0);
        CHECK(rz > 0.0 && rz <= kMaxStretchRatio);

        // The clamp itself, and the "garbage means no stretch" rule.
        CHECK(ClampStretchRatio(1.0) == 1.0);
        CHECK(ClampStretchRatio(0.1) == kMinStretchRatio);
        CHECK(ClampStretchRatio(9.0) == kMaxStretchRatio);
        CHECK(ClampStretchRatio(0.0) == 1.0);
        CHECK(ClampStretchRatio(-2.0) == 1.0);
        CHECK(ClampStretchRatio(std::nan("")) == 1.0);

        // And the model predicates the engine and the exporter key off. A ratio
        // a rounding away from 1 is 1: it must not put the clip on the stretch
        // path (different code, and a badge drawn on the clip for nothing).
        Clip f;
        CHECK(!ClipIsStretched(f));
        f.stretchRatio = 1.0 + 1e-15;
        CHECK(!ClipIsStretched(f));
        f.stretchRatio = 1.0 + 1e-9;
        CHECK(!ClipIsStretched(f));
        f.stretchRatio = 1.001;
        CHECK(ClipIsStretched(f));
        f.stretchRatio = 1.25;
        CHECK(ClipIsStretched(f));
        // An out-of-range value in the model is read through the clamp, so a
        // clip can never be "stretched" at a ratio the DSP cannot do.
        f.stretchRatio = 100.0;
        CHECK(std::fabs(ClipStretchRatio(f) - kMaxStretchRatio) < 1e-12);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
