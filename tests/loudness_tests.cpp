// Host-buildable tests for the BS.1770-4 loudness meter (daw::Loudness).
//
// These are deliberately tolerant: the goal is ballpark correctness and
// monotonic behavior, not sample-exact conformance. We check that
//   - silence reads a finite floor (never -inf) and a very low integrated;
//   - a 1 kHz sine at amplitude 0.1 (both channels) lands in a sane LUFS
//     range around -20 LUFS after K-weighting + stereo summation;
//   - the reported true peak is always >= the raw sample peak;
//   - a louder sine reads higher than a quieter one (momentary + integrated).
//
// Build/run:
//   g++ -std=c++17 -Isrc src/dsp/Loudness.cpp tests/loudness_tests.cpp
//       -o /tmp/loud && /tmp/loud

#include "../src/dsp/Loudness.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Interleaved-stereo 1 kHz sine of the given amplitude on both channels.
static std::vector<float> sine(int frames, double sr, double amp,
                               double freqHz = 1000.0) {
    std::vector<float> buf(frames * 2);
    const double w = 2.0 * M_PI * freqHz / sr;
    for (int i = 0; i < frames; ++i) {
        const float v = static_cast<float>(amp * std::sin(w * i));
        buf[i * 2 + 0] = v;
        buf[i * 2 + 1] = v;
    }
    return buf;
}

int main() {
    const double SR = 48000.0;

    // 1. Silence: finite floor everywhere, integrated very low (no -inf).
    {
        Loudness m;
        m.Prepare(SR);
        std::vector<float> buf(SR * 2 /*frames*/ * 2 /*chans*/, 0.0f);
        m.Process(buf.data(), static_cast<int>(SR * 2));
        CHECK(std::isfinite(m.IntegratedLufs()));
        CHECK(std::isfinite(m.MomentaryLufs()));
        CHECK(std::isfinite(m.TruePeakDb()));
        CHECK(m.IntegratedLufs() < -50.0f);   // effectively silent
        CHECK(m.MomentaryLufs() < -50.0f);
    }

    // 2. 1 kHz sine at amplitude 0.1 (-20 dBFS peak) -> ~-20 LUFS integrated.
    //    Generous bounds: BS.1770 stereo summation puts this near -20 LUFS.
    {
        Loudness m;
        m.Prepare(SR);
        auto buf = sine(static_cast<int>(SR * 2), SR, 0.1);
        m.Process(buf.data(), static_cast<int>(SR * 2));
        const float integ = m.IntegratedLufs();
        CHECK(std::isfinite(integ));
        CHECK(integ > -27.0f && integ < -14.0f);   // ballpark, tolerant
        // Short-term over a steady 2 s tone tracks the same ballpark.
        CHECK(m.ShortTermLufs() > -27.0f && m.ShortTermLufs() < -14.0f);
    }

    // 3. True peak >= sample peak for a real signal.
    {
        Loudness m;
        m.Prepare(SR);
        auto buf = sine(static_cast<int>(SR * 1), SR, 0.5);
        float samplePeak = 0.0f;
        for (float v : buf) samplePeak = std::max(samplePeak, std::fabs(v));
        m.Process(buf.data(), static_cast<int>(SR * 1));
        const float tpLinear = std::pow(10.0f, m.TruePeakDb() / 20.0f);
        CHECK(tpLinear >= samplePeak * 0.999f);   // never below sample peak
        // A 0.5-amplitude signal peaks near -6 dBTP, comfortably below 0.
        CHECK(m.TruePeakDb() > -8.0f && m.TruePeakDb() < 0.0f);
    }

    // 4. Louder reads higher than quieter (monotonic), momentary + integrated.
    {
        Loudness loud, quiet;
        loud.Prepare(SR);
        quiet.Prepare(SR);
        auto bLoud  = sine(static_cast<int>(SR * 2), SR, 0.20);
        auto bQuiet = sine(static_cast<int>(SR * 2), SR, 0.02);
        loud.Process(bLoud.data(), static_cast<int>(SR * 2));
        quiet.Process(bQuiet.data(), static_cast<int>(SR * 2));
        CHECK(loud.IntegratedLufs() > quiet.IntegratedLufs() + 5.0f);
        CHECK(loud.MomentaryLufs() > quiet.MomentaryLufs() + 5.0f);
        CHECK(loud.TruePeakDb()    > quiet.TruePeakDb()    + 5.0f);
    }

    // 5. Reset clears everything back to the silent floor.
    {
        Loudness m;
        m.Prepare(SR);
        auto buf = sine(static_cast<int>(SR * 1), SR, 0.3);
        m.Process(buf.data(), static_cast<int>(SR * 1));
        CHECK(m.TruePeakDb() > -20.0f);   // saw signal
        m.Reset();
        CHECK(m.IntegratedLufs() < -50.0f);
        CHECK(m.TruePeakDb() < -50.0f);
    }

    // 6. True-peak FIR: every phase has unit DC gain (~1.0). Catches the
    //    earlier bad phase-2 (DC 0.862) directly, not via the sample-peak floor.
    {
        Loudness m;
        m.Prepare(SR);
        for (int p = 0; p < 4; ++p)
            CHECK(std::fabs(m.TpPhaseDcGain(p) - 1.0) < 1e-6);
    }

    // 7. A default-constructed meter (no Prepare) survives Process() and reads
    //    the silent floor — no out-of-bounds on the empty windows.
    {
        Loudness m;   // not Prepared
        auto buf = sine(1000, SR, 0.5);
        m.Process(buf.data(), 1000);   // must not crash
        CHECK(std::isfinite(m.IntegratedLufs()));
        CHECK(std::isfinite(m.MomentaryLufs()));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
