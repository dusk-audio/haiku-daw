// Host-buildable tests for the extra built-in DSP effects (Reverb, Compressor).
// Reverb: a single impulse leaves a decaying tail. Compressor: a loud signal
// above threshold comes out with lower peak than a quiet below-threshold signal
// (which passes near unity).

#include "../src/dsp/Reverb.h"
#include "../src/dsp/Compressor.h"

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

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
