// Host-buildable tests for the Gate (downward expander / noise gate) effect.
// Checks that a signal above threshold passes ~unchanged, a signal below
// threshold is attenuated toward the range floor, a signal well below threshold
// approaches the full range attenuation, and Reset() clears state.

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

// Interleaved-stereo constant-magnitude buffer (a "DC" test tone by peak level).
static std::vector<float> dc(int frames, float v) {
    return std::vector<float>(frames * 2, v);
}

// Peak magnitude of the last frame in an interleaved-stereo buffer.
static float lastPeak(const std::vector<float>& buf, int frames) {
    float l = std::fabs(buf[(frames - 1) * 2 + 0]);
    float r = std::fabs(buf[(frames - 1) * 2 + 1]);
    return l > r ? l : r;
}

int main() {
    const double SR = 48000.0;

    // Threshold -20 dB ~= 0.1 linear. Fast attack/release so state settles well
    // within the buffers we feed.
    const double threshDb = -20.0;    // ~0.1 linear
    const double ratio    = 4.0;
    const double attackMs = 1.0;
    const double relMs    = 5.0;
    const double rangeDb  = 40.0;     // floor ~= 0.01 linear

    // 1. Signal ABOVE threshold passes ~unchanged (gate open, unity gain).
    {
        Gate g(threshDb, ratio, attackMs, relMs, rangeDb);
        g.Prepare(SR);
        const float in = 0.5f;   // -6 dB, well above -20 dB threshold
        auto buf = dc(4000, in);
        g.Process(buf.data(), 4000);
        CHECK(std::fabs(lastPeak(buf, 4000) - in) < 0.01f);
    }

    // 2. Signal BELOW threshold is attenuated: peak out < peak in.
    {
        Gate g(threshDb, ratio, attackMs, relMs, rangeDb);
        g.Prepare(SR);
        const float in = 0.03f;  // ~-30 dB, below -20 dB threshold
        auto buf = dc(8000, in);
        g.Process(buf.data(), 8000);
        const float out = lastPeak(buf, 8000);
        CHECK(out < in);                 // attenuated
        CHECK(out < in * 0.9f);          // meaningfully so, toward the floor
    }

    // 3. Signal WELL below threshold approaches the range attenuation (floor).
    {
        Gate g(threshDb, ratio, attackMs, relMs, rangeDb);
        g.Prepare(SR);
        const float in = 0.001f; // ~-60 dB; (threshold-level)*3 exceeds range
        auto buf = dc(8000, in);
        g.Process(buf.data(), 8000);
        const float out = lastPeak(buf, 8000);
        // Full range attenuation is 40 dB (0.01x): output ~= in * floor.
        const float floorLin = std::pow(10.0f, -rangeDb / 20.0f);   // 0.01
        const float expected = in * floorLin;
        CHECK(std::fabs(out - expected) < expected * 0.2f + 1e-7f);
    }

    // 4. Reset() clears state: after driving the gate closed on a quiet signal,
    //    Reset() reopens it so a fresh loud burst starts from unity gain.
    {
        Gate g(threshDb, ratio, attackMs, relMs, rangeDb);
        g.Prepare(SR);
        // Drive it closed with a quiet signal.
        auto quiet = dc(8000, 0.001f);
        g.Process(quiet.data(), 8000);
        // Reset should restore the open (unity) envelope.
        g.Reset();
        // A loud signal should pass at ~unity from the very first frames.
        auto loud = dc(64, 0.5f);
        g.Process(loud.data(), 64);
        CHECK(std::fabs(lastPeak(loud, 64) - 0.5f) < 0.01f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
