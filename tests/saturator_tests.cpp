// Host-buildable tests for the Saturator (tape/FET saturation) effect.
//
// Ported DSP: tape-echo's softClip cubic waveshaper + preampShape asymmetric
// FET/bias character + drive taper + per-channel DC blocker. The checks below
// exercise the transfer curve (sublinear at high level, ~linear at tiny level),
// the dry/wet mix, and Reset() clearing the DC-blocker state.

#include "../src/dsp/Saturator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Interleaved-stereo sine at `freq` Hz, amplitude `amp`, `frames` long.
static std::vector<float> sine(int frames, double freq, double amp, double SR) {
    std::vector<float> buf(frames * 2);
    const double w = 2.0 * 3.14159265358979323846 * freq / SR;
    for (int i = 0; i < frames; i++) {
        float v = static_cast<float>(amp * std::sin(w * i));
        buf[i * 2 + 0] = v;
        buf[i * 2 + 1] = v;
    }
    return buf;
}

static float peakAbs(const std::vector<float>& b) {
    float p = 0.0f;
    for (float v : b) p = std::max(p, std::fabs(v));
    return p;
}

int main() {
    const double SR = 48000.0;

    // 1. Drive up: a loud input is soft-clipped (transfer curve sublinear at
    //    high level). Wet-only output peak stays below what the curve's
    //    small-signal gain would project for the same loud input.
    {
        Saturator s(1.0, 1.0, 0.0);   // max drive, fully wet
        s.Prepare(SR);

        // Small-signal gain of the wet path: the shaper slope at 0 is 1, so the
        // linear gain a tiny signal sees is driveMult*driveComp. Measure it.
        auto tiny = sine(2000, 1000.0, 1.0e-4, SR);
        s.Process(tiny.data(), 2000);
        const float g0 = peakAbs(tiny) / 1.0e-4f;   // ~ driveMult*driveComp

        s.Reset();
        auto loud = sine(2000, 1000.0, 0.9, SR);
        s.Process(loud.data(), 2000);
        const float loudPeak = peakAbs(loud);

        // Sublinear: saturating 0.9 yields less than a pure linear gain would.
        CHECK(loudPeak < g0 * 0.9f);
        CHECK(loudPeak > 0.0f);          // still passes signal
    }

    // 2. Tiny input passes ~linearly (near unity through the curve) at low
    //    drive. The bounded cubic is locally linear near 0 and the drive
    //    compensation keeps the low-drive gain close to unity.
    {
        Saturator s(0.0, 1.0, 0.0);   // low drive, fully wet
        s.Prepare(SR);
        auto tiny = sine(2000, 1000.0, 1.0e-4, SR);
        const float inPeak = peakAbs(tiny);
        s.Process(tiny.data(), 2000);
        const float outPeak = peakAbs(tiny);
        CHECK(std::fabs(outPeak / inPeak - 1.0f) < 0.1f);   // near unity
    }

    // 3. mix = 0 is fully dry: output == input, sample for sample.
    {
        Saturator s(1.0, 0.0, 6.0);   // heavy drive + trim, but mix 0
        s.Prepare(SR);
        auto in  = sine(512, 440.0, 0.7, SR);
        auto buf = in;
        s.Process(buf.data(), 512);
        bool identical = true;
        for (size_t i = 0; i < buf.size(); i++)
            if (std::fabs(buf[i] - in[i]) > 1.0e-6f) { identical = false; break; }
        CHECK(identical);
    }

    // 4. Reset() clears DC-blocker state: after driving a loud burst and
    //    resetting, the same input produces the same output as a fresh instance.
    {
        Saturator fresh(0.8, 1.0, 0.0);
        fresh.Prepare(SR);
        auto probe = sine(1000, 1000.0, 0.5, SR);
        auto refBuf = probe;
        fresh.Process(refBuf.data(), 1000);

        Saturator used(0.8, 1.0, 0.0);
        used.Prepare(SR);
        auto burst = sine(4000, 90.0, 1.5, SR);   // load DC-blocker memory
        used.Process(burst.data(), 4000);
        used.Reset();
        auto testBuf = probe;
        used.Process(testBuf.data(), 1000);

        bool matches = true;
        for (size_t i = 0; i < testBuf.size(); i++)
            if (std::fabs(testBuf[i] - refBuf[i]) > 1.0e-5f) { matches = false; break; }
        CHECK(matches);
    }

    // 5. Reset() from a fresh state is a no-op that leaves the curve working.
    {
        Saturator s(0.6, 1.0, 0.0);
        s.Prepare(SR);
        s.Reset();
        auto buf = sine(256, 1000.0, 0.3, SR);
        s.Process(buf.data(), 256);
        CHECK(peakAbs(buf) > 0.0f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
