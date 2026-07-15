// Host-buildable tests for the extra built-in DSP effects (Reverb, Compressor).
// Reverb: a single impulse leaves a decaying tail. Compressor: a loud signal
// above threshold comes out with lower peak than a quiet below-threshold signal
// (which passes near unity).

#include "../src/dsp/Reverb.h"
#include "../src/dsp/Compressor.h"
#include "../src/dsp/Eq.h"
#include "../src/dsp/Delay.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;

    // 1b. DuskVerb plate engine: impulse -> a decaying, non-silent tail.
    {
        Reverb rv(0.7, 1.0);
        rv.SetParam(2, 1.0f);   // algorithm = DuskPlate
        rv.SetParam(3, 2.5f);   // decay 2.5 s
        rv.Prepare(SR);         // builds the plate engine
        const int N = 48000;    // 1 s
        std::vector<float> buf(N * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        rv.Process(buf.data(), N);
        float lateEnergy = 0.0f;
        for (int i = 12000; i < N; i++)
            lateEnergy += std::fabs(buf[i * 2]) + std::fabs(buf[i * 2 + 1]);
        CHECK(lateEnergy > 1e-3f);   // plate tail exists well after the impulse
        // Not NaN/Inf.
        bool finite = true;
        for (int i = 0; i < N * 2; i++) if (!std::isfinite(buf[i])) finite = false;
        CHECK(finite);
    }

    // 1c. DuskVerb hall + FDN engines: impulse -> finite decaying tail.
    for (float algo : { 2.0f, 3.0f }) {   // 2 = DuskHall, 3 = DuskFDN
        Reverb rv(0.7, 1.0);
        rv.SetParam(2, algo);
        rv.SetParam(3, 3.0f);
        rv.Prepare(SR);
        const int N = 48000;
        std::vector<float> buf(N * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        rv.Process(buf.data(), N);
        float lateEnergy = 0.0f; bool finite = true;
        for (int i = 0; i < N * 2; i++) {
            if (!std::isfinite(buf[i])) finite = false;
            if (i >= 24000) lateEnergy += std::fabs(buf[i]);
        }
        CHECK(finite);
        CHECK(lateEnergy > 1e-3f);
    }

    // 1. Reverb: impulse then silence yields a decaying, non-silent tail.
    {
        Reverb rv(0.7, 1.0);   // large room, fully wet so the tail is visible
        rv.Prepare(SR);

        const int N = 24000;   // 0.5 s
        std::vector<float> buf(N * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;   // single impulse at frame 0
        rv.Process(buf.data(), N);

        // Energy well after the impulse (past the longest comb) must be
        // non-zero: a reverb tail exists.
        float lateEnergy = 0.0f;
        for (int i = 8000; i < N; i++)
            lateEnergy += std::fabs(buf[i * 2]) + std::fabs(buf[i * 2 + 1]);
        CHECK(lateEnergy > 1e-3f);

        // A specific late sample should be non-zero somewhere in a window
        // (decaying tail, not a single spike).
        float win = 0.0f;
        for (int i = 12000; i < 12500; i++)
            win += std::fabs(buf[i * 2]);
        CHECK(win > 1e-5f);
    }

    // 2. Reverb Reset() clears the tail: a fresh impulse behaves the same.
    {
        Reverb rv(0.7, 1.0);
        rv.Prepare(SR);
        std::vector<float> a(4000 * 2, 0.0f);
        a[0] = 1.0f; a[1] = 1.0f;
        rv.Process(a.data(), 4000);

        rv.Reset();
        std::vector<float> b(4000 * 2, 0.0f);
        b[0] = 1.0f; b[1] = 1.0f;
        rv.Process(b.data(), 4000);

        // After Reset the response is identical to the first run.
        float diff = 0.0f;
        for (size_t i = 0; i < a.size(); i++)
            diff += std::fabs(a[i] - b[i]);
        CHECK(diff < 1e-4f);
    }

    // 3. Compressor: loud (above threshold) is attenuated; quiet (below) is not.
    {
        // Threshold -20 dB ~= 0.1 linear, 4:1 ratio, fast attack, no makeup.
        Compressor comp(-20.0, 4.0, 5.0, 50.0, 0.0);
        comp.Prepare(SR);

        // Loud steady signal at 0.8 (~ -1.9 dB, well above threshold).
        const int N = 24000;
        std::vector<float> loud(N * 2, 0.8f);
        comp.Process(loud.data(), N);
        // Look at the settled tail (after attack) for the compressed peak.
        float loudPeak = 0.0f;
        for (int i = N - 1000; i < N; i++) {
            loudPeak = std::max(loudPeak, std::fabs(loud[i * 2]));
            loudPeak = std::max(loudPeak, std::fabs(loud[i * 2 + 1]));
        }
        CHECK(loudPeak < 0.8f - 0.05f);   // meaningfully reduced
        CHECK(comp.MeterDb() < -0.5f);    // GR meter shows reduction

        // Quiet steady signal at 0.02 (~ -34 dB, below threshold): near unity.
        comp.Reset();
        std::vector<float> quiet(N * 2, 0.02f);
        comp.Process(quiet.data(), N);
        float quietPeak = 0.0f;
        for (int i = N - 1000; i < N; i++) {
            quietPeak = std::max(quietPeak, std::fabs(quiet[i * 2]));
            quietPeak = std::max(quietPeak, std::fabs(quiet[i * 2 + 1]));
        }
        CHECK(std::fabs(quietPeak - 0.02f) < 0.001f);   // essentially unchanged
    }

    // 3b. EQ FFT analyzer: a 1 kHz sine puts the most energy in the bin nearest
    // 1 kHz, well above a far-away bin.
    {
        Eq eq; eq.Prepare(SR);
        const double binHz = SR / (double)Eq::kFftSize;
        const int N = Eq::kFftSize * 3;   // fill the FFT buffer a few times
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; i++) {
            const float s = 0.5f * (float)std::sin(2.0 * M_PI * 1000.0 * i / SR);
            buf[i * 2] = buf[i * 2 + 1] = s;
        }
        eq.Process(buf.data(), N);
        float spec[Eq::kBins];
        const int n = eq.Spectrum(spec, Eq::kBins);
        CHECK(n == Eq::kBins);
        const int kTone = (int)(1000.0 / binHz + 0.5);
        const int kFar  = (int)(8000.0 / binHz + 0.5);
        CHECK(spec[kTone] > spec[kFar] + 20.0f);   // tone bin dominates (dB)
    }

    // 4. Compressor makeup gain lifts a below-threshold signal.
    {
        Compressor comp(-20.0, 4.0, 5.0, 50.0, 6.0);   // +6 dB makeup
        comp.Prepare(SR);
        const int N = 8000;
        std::vector<float> buf(N * 2, 0.02f);
        comp.Process(buf.data(), N);
        // ~ +6 dB => ~2x; below threshold so only makeup applies.
        CHECK(buf[2 * (N - 1)] > 0.03f);
    }

    // 3. EQ: the low-shelf band (80 Hz) lifts DC; a flat EQ is a near no-op;
    // the high-shelf band leaves DC ~unchanged.
    {
        // Low-shelf +12 dB on band 0 -> DC (0 Hz) rises ~4x (10^(12/20)=3.98).
        Eq low; low.SetBand(0, 80.0f, 12.0f, 0.7f);
        low.Prepare(SR);
        std::vector<float> buf(4000 * 2, 1.0f);      // DC
        low.Process(buf.data(), 4000);
        CHECK(std::fabs(buf[2 * 3999] - 3.98f) < 0.3f);

        // Flat EQ: DC passes through ~unity.
        Eq flat;
        flat.Prepare(SR);
        std::vector<float> b2(4000 * 2, 1.0f);
        flat.Process(b2.data(), 4000);
        CHECK(std::fabs(b2[2 * 3999] - 1.0f) < 1e-3f);

        // High-shelf +12 dB (band 4): DC (well below 6.5 kHz) ~unchanged.
        Eq high; high.SetBand(4, 6500.0f, 12.0f, 0.7f);
        high.Prepare(SR);
        std::vector<float> b3(4000 * 2, 1.0f);
        high.Process(b3.data(), 4000);
        CHECK(std::fabs(b3[2 * 3999] - 1.0f) < 0.1f);
    }

    // SetParam (per-block automation hook): setting the EQ mid band's gain via
    // SetParam matches SetBand; setting a compressor param stays finite.
    {
        Eq a; a.SetBand(2, 1000.0f, 9.0f, 2.0f); a.Prepare(SR);
        Eq b; b.Prepare(SR);
        b.SetParam(2 * 3 + 0, 1000.0f);   // mid freq
        b.SetParam(2 * 3 + 1, 9.0f);      // mid gain
        b.SetParam(2 * 3 + 2, 2.0f);      // mid Q
        CHECK(std::fabs(a.MagnitudeResponseDb(1000.0f)
                        - b.MagnitudeResponseDb(1000.0f)) < 0.01f);
        Compressor c(-18, 4, 5, 80, 0); c.Prepare(SR);
        c.SetParam(0, -30.0f);            // threshold
        std::vector<float> buf(1000 * 2, 0.5f);
        c.Process(buf.data(), 1000);
        bool fin = true; for (float v : buf) if (!std::isfinite(v)) fin = false;
        CHECK(fin);
    }

    // EQ magnitude response: flat EQ ~ 0 dB everywhere; a +9 dB mid-peak reads
    // near +9 dB at its center and ~0 dB far away. (Drives the graph UI.)
    {
        Eq flat; flat.Prepare(SR);
        CHECK(std::fabs(flat.MagnitudeResponseDb(1000.0f)) < 0.5f);
        Eq peak; peak.SetBand(2, 1000.0f, 9.0f, 2.0f); peak.Prepare(SR);
        peak.SetBand(2, 1000.0f, 9.0f, 2.0f);   // re-apply after Prepare reset
        CHECK(peak.MagnitudeResponseDb(1000.0f) > 6.0f);
        CHECK(std::fabs(peak.MagnitudeResponseDb(60.0f)) < 2.0f);
    }

    // Regression: a high-Q shelf with gain must NOT produce NaN (radicand went
    // negative in the shelf-alpha formula for Q > 1).
    {
        Eq eq; eq.SetBand(0, 80.0f, 12.0f, 10.0f);   // low shelf, +12 dB, Q=10
        eq.Prepare(SR);
        std::vector<float> buf(2000 * 2, 0.3f);
        eq.Process(buf.data(), 2000);
        bool allFinite = true;
        for (float v : buf) if (!std::isfinite(v)) allFinite = false;
        CHECK(allFinite);
    }

    // Regression: a >= 1.0 delay feedback must stay finite (clamped, no Inf).
    {
        Delay dl(0.01, 1.5 /*feedback*/, 0.5);   // 1.5 would diverge unclamped
        dl.Prepare(SR);
        std::vector<float> buf(20000 * 2, 0.0f);
        buf[0] = buf[1] = 1.0f;                  // an impulse to feed the loop
        dl.Process(buf.data(), 20000);
        bool allFinite = true;
        for (float v : buf) if (!std::isfinite(v)) allFinite = false;
        CHECK(allFinite);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
