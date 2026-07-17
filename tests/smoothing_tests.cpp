// Host-buildable regression tests for parameter-change smoothing (zipper-noise
// fix). Each built-in effect, when a parameter is STEPPED mid-stream (as
// per-block automation does), must ramp the change in over ~10 ms rather than
// snapping it — otherwise the coefficient/gain discontinuity clicks.
//
// The shared assertion pattern per effect: feed a steady tone, let it settle,
// step one parameter, then over the post-step region check
//   (1) the change actually took effect      (converged: late != pre-step),
//   (2) it was NOT applied instantly          (early still near the old value),
//   (3) no click                              (bounded sample-to-sample delta).
// An un-smoothed (snapping) implementation fails (2) and/or (3).

#include "../src/dsp/Biquad.h"
#include "../src/dsp/Eq.h"
#include "../src/dsp/Compressor.h"
#include "../src/dsp/Gate.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Interleaved-stereo sine block starting at absolute sample index n0 (so phase
// stays continuous across successive blocks).
static std::vector<float> Sine(int n0, int frames, double freq, double amp, double sr) {
    std::vector<float> b(frames * 2);
    for (int i = 0; i < frames; ++i) {
        const float s = (float)(amp * std::sin(2.0 * M_PI * freq * (n0 + i) / sr));
        b[i * 2 + 0] = s;
        b[i * 2 + 1] = s;
    }
    return b;
}

static float PeakL(const std::vector<float>& b, int from, int to) {
    float pk = 0.0f;
    for (int i = from; i < to; ++i) pk = std::max(pk, std::fabs(b[i * 2]));
    return pk;
}

// Max sample-to-sample delta of the left channel over [from, to).
static float MaxStepL(const std::vector<float>& b, int from, int to) {
    float m = 0.0f;
    for (int i = std::max(1, from); i < to; ++i)
        m = std::max(m, std::fabs(b[i * 2] - b[(i - 1) * 2]));
    return m;
}
static float MaxStepL(const std::vector<float>& b) {
    return MaxStepL(b, 1, (int)(b.size() / 2));
}

int main() {
    const double SR = 48000.0;

    // --- Biquad: step the low-pass cutoff from ~passthrough down to 200 Hz on a
    // steady 1 kHz tone. The 1 kHz level must fall gradually, not click. --------
    {
        Biquad f(Biquad::Type::LowPass, 20000.0, 0.707, 0.0);
        f.Prepare(SR);
        auto settle = Sine(0, 4800, 1000.0, 1.0, SR);
        f.Process(settle.data(), 4800);
        const float prePeak = PeakL(settle, 4560, 4800);   // ~unity before step

        f.SetParam(1, 200.0f);                              // cutoff 20k -> 200 Hz
        auto post = Sine(4800, 12000, 1000.0, 1.0, SR);
        f.Process(post.data(), 12000);

        const float early = PeakL(post, 0, 240);            // first 5 ms
        const float late  = PeakL(post, 11000, 12000);      // settled
        CHECK(prePeak > 0.7f);                              // passed before the step
        CHECK(late < 0.4f);                                 // LP-200 killed 1 kHz (converged)
        CHECK(early > 0.6f);                                // still loud just after step (gradual)
        // Boundary continuity is the click signature: a coefficient SNAP spikes
        // the first sample or two (mismatched DF1 state) to ~1.0; the glide keeps
        // it at the tone's natural slope (~0.13). (A later ~0.4 filter-sweep
        // transient as the resonant LP crosses 1 kHz is inherent, not a click.)
        CHECK(MaxStepL(post, 1, 32) < 0.25f);
    }

    // --- Eq: step band-2 (peak @ 750 Hz) gain 0 -> +12 dB on a 750 Hz tone. The
    // level must rise gradually. -------------------------------------------------
    {
        Eq e;
        e.Prepare(SR);
        auto settle = Sine(0, 4800, 750.0, 0.5, SR);
        e.Process(settle.data(), 4800);

        e.SetParam(2 * 3 + 1, 12.0f);                       // band 2 gain -> +12 dB
        auto post = Sine(4800, 12000, 750.0, 0.5, SR);
        e.Process(post.data(), 12000);

        const float early = PeakL(post, 0, 240);
        const float late  = PeakL(post, 11000, 12000);
        CHECK(late > 1.5f);                                 // +12 dB took effect (0.5*3.98)
        CHECK(early < late * 0.7f);                         // ramped up, not instant
        CHECK(MaxStepL(post) < 0.4f);                       // no click
    }

    // --- Compressor: step makeup gain 0 -> +12 dB below threshold (no
    // compression), isolating the makeup smoothing. Level rises gradually. ------
    {
        Compressor c(-20.0, 4.0, 10.0, 100.0, 0.0);
        c.Prepare(SR);
        auto settle = Sine(0, 2400, 1000.0, 0.05, SR);      // -26 dB, below -20 thresh
        c.Process(settle.data(), 2400);

        c.SetParam(4, 12.0f);                               // makeup -> +12 dB
        auto post = Sine(2400, 4800, 1000.0, 0.05, SR);
        c.Process(post.data(), 4800);

        const float early = PeakL(post, 0, 120);
        const float late  = PeakL(post, 3800, 4800);
        CHECK(late > 0.15f);                                // 0.05 * 3.98 (converged)
        CHECK(early < late * 0.7f);                         // ramped up, not instant
        CHECK(MaxStepL(post) < 0.05f);                      // no click (a snap steps ~0.15)
    }

    // --- Gate: with the gate held closed at a deep range floor, step range
    // 60 -> 0 dB (floor 0.001 -> 1.0). The clamp would otherwise snap the gain
    // open in one sample; the floor glide must ramp it. ------------------------
    {
        Gate g(-20.0, 100.0, 1.0, 100.0, 60.0);
        g.Prepare(SR);
        // Below-threshold tone -> gate slams closed to the range floor. Settle
        // long (release 100 ms) so the envelope reaches the floor.
        for (int blk = 0; blk < 10; ++blk) {
            auto s = Sine(blk * 2400, 2400, 100.0, 0.02, SR);   // -34 dB, below thresh
            g.Process(s.data(), 2400);
        }
        g.SetParam(4, 0.0f);                                // range 60 -> 0 dB (floor -> 1.0)
        auto post = Sine(10 * 2400, 12000, 100.0, 0.02, SR);
        g.Process(post.data(), 12000);

        const float early = PeakL(post, 0, 240);
        const float late  = PeakL(post, 11000, 12000);
        CHECK(late > 0.015f);                               // floor opened the gate (~0.02)
        CHECK(early < late * 0.7f);                         // ramped, not a one-sample snap
        CHECK(MaxStepL(post) < 0.005f);                     // no click (a snap steps ~0.02)
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
