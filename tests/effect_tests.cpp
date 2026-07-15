// Host-buildable tests for the built-in DSP effects. Checks the Biquad's
// DC/Nyquist behavior (low-pass passes DC, high-pass blocks it) and the
// Delay's echo timing + feedback.

#include "../src/dsp/Biquad.h"
#include "../src/dsp/Delay.h"
#include "../src/dsp/EffectFactory.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Interleaved stereo helpers.
static std::vector<float> dc(int frames, float v) {
    return std::vector<float>(frames * 2, v);
}

int main() {
    const double SR = 48000.0;

    // 1. Low-pass passes DC: constant input settles to ~itself.
    {
        Biquad lp(Biquad::Type::LowPass, 1000.0, 0.707);
        lp.Prepare(SR);
        auto buf = dc(2000, 1.0f);
        lp.Process(buf.data(), 2000);
        // After settling, output ~= input (unity DC gain).
        CHECK(std::fabs(buf[2 * 1999] - 1.0f) < 0.01f);
    }

    // 2. High-pass blocks DC: constant input settles to ~0.
    {
        Biquad hp(Biquad::Type::HighPass, 1000.0, 0.707);
        hp.Prepare(SR);
        auto buf = dc(4000, 1.0f);
        hp.Process(buf.data(), 4000);
        CHECK(std::fabs(buf[2 * 3999]) < 0.01f);
    }

    // 3. Low-pass attenuates Nyquist (alternating +1/-1 has less energy out).
    {
        Biquad lp(Biquad::Type::LowPass, 1000.0, 0.707);
        lp.Prepare(SR);
        std::vector<float> buf(400 * 2);
        for (int i = 0; i < 400; i++) {
            float v = (i & 1) ? -1.0f : 1.0f;
            buf[i * 2] = v; buf[i * 2 + 1] = v;
        }
        lp.Process(buf.data(), 400);
        CHECK(std::fabs(buf[2 * 399]) < 0.1f);   // heavily attenuated
    }

    // 4. Delay: dry-only impulse echoes after delayFrames with feedback.
    {
        Delay d(4.0 / SR, 0.0, 1.0);   // 4-frame delay, no feedback, fully wet
        d.Prepare(SR);
        std::vector<float> buf(16 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;  // impulse at frame 0
        d.Process(buf.data(), 16);
        // Fully wet + 4-frame delay: energy appears at frame 4, not frame 0.
        CHECK(std::fabs(buf[0]) < 1e-6f);
        CHECK(std::fabs(buf[2 * 4] - 1.0f) < 1e-4f);
        CHECK(std::fabs(buf[2 * 8]) < 1e-4f);   // no feedback -> no 2nd echo
    }

    // 5. Delay feedback produces a decaying second echo.
    {
        Delay d(4.0 / SR, 0.5, 1.0);   // 50% feedback
        d.Prepare(SR);
        std::vector<float> buf(16 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        d.Process(buf.data(), 16);
        CHECK(std::fabs(buf[2 * 4] - 1.0f) < 1e-4f);   // first echo
        CHECK(std::fabs(buf[2 * 8] - 0.5f) < 1e-4f);   // second echo * feedback
    }

    // 5b. Delay tempo-sync: 1/4 note at 120 BPM = 0.5 s echo (24000 frames).
    {
        Delay d(0.25, 0.0, 1.0, /*sync*/ true, /*division 1/4*/ 0);
        d.Prepare(SR);
        d.SetTempo(120.0);
        std::vector<float> buf(30000 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        d.Process(buf.data(), 30000);
        CHECK(std::fabs(buf[2 * 24000] - 1.0f) < 1e-4f);   // echo at 0.5 s
        CHECK(std::fabs(buf[2 * 12000]) < 1e-4f);          // nothing at 0.25 s
    }

    // 5c. Tempo change moves the synced echo: 1/4 at 60 BPM = 1.0 s (48000).
    {
        Delay d(0.25, 0.0, 1.0, true, 0);
        d.Prepare(SR);
        d.SetTempo(60.0);
        std::vector<float> buf(50000 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        d.Process(buf.data(), 50000);
        CHECK(std::fabs(buf[2 * 48000] - 1.0f) < 1e-4f);
    }

    // 5d. Eighth-note division: 1/8 at 120 BPM = 0.25 s (12000 frames).
    {
        Delay d(0.25, 0.0, 1.0, true, 3);   // division index 3 = "1/8"
        d.Prepare(SR);
        d.SetTempo(120.0);
        std::vector<float> buf(16000 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        d.Process(buf.data(), 16000);
        CHECK(std::fabs(buf[2 * 12000] - 1.0f) < 1e-4f);
    }

    // 5e. Live time change via SetParam(0) (movable tap, no reallocation).
    {
        Delay d(0.1, 0.0, 1.0);            // free mode, 0.1 s
        d.Prepare(SR);
        d.SetParam(0, 8.0f / (float)SR);   // retune to an 8-frame delay live
        std::vector<float> buf(16 * 2, 0.0f);
        buf[0] = 1.0f; buf[1] = 1.0f;
        d.Process(buf.data(), 16);
        CHECK(std::fabs(buf[2 * 8] - 1.0f) < 1e-4f);
    }

    // 5f. Division labels.
    {
        CHECK(std::strcmp(Delay::DivisionName(0), "1/4") == 0);
        CHECK(std::strcmp(Delay::DivisionName(3), "1/8") == 0);
    }

    // 6. Factory builds the right effects from descriptors and they run.
    {
        auto lp = MakeEffect(LowPassDesc(1000.0f));
        CHECK(lp != nullptr);
        auto dl = MakeEffect(DelayDesc());
        CHECK(dl != nullptr);
        lp->Prepare(SR);
        auto buf = dc(2000, 1.0f);
        lp->Process(buf.data(), 2000);
        CHECK(std::fabs(buf[2 * 1999] - 1.0f) < 0.01f);   // low-pass passes DC
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
