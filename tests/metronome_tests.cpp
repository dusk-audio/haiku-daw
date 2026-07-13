// Host-buildable tests for the Metronome: a tick has energy right after each
// beat boundary and silence between ticks, split rendering matches whole-block
// rendering (deterministic), and the bar downbeat differs from a plain beat.

#include "../src/engine/Metronome.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static float rms(const std::vector<float>& b, size_t from, size_t to) {
    double s = 0; size_t n = 0;
    for (size_t i = from; i < to && i < b.size(); i++) { s += (double)b[i]*b[i]; n++; }
    return n ? (float)std::sqrt(s / n) : 0.0f;
}

int main() {
    const double SR = 48000.0;
    Metronome m(SR, 120.0, 4);        // 24000 frames/beat
    CHECK(m.FramesPerBeat() == 24000.0);

    // Render one beat's worth from frame 0.
    std::vector<float> buf(24000 * 2, 0.0f);
    m.Render(buf.data(), 24000, 0, 1.0f);
    // Energy in the first ~30 ms (the tick), silence well after it.
    CHECK(rms(buf, 0, 1440 * 2) > 0.01f);              // first 30 ms: tick
    CHECK(rms(buf, 5000 * 2, 20000 * 2) < 1e-4f);      // mid-beat: silence

    // Split rendering == whole rendering (deterministic, stateless).
    {
        std::vector<float> whole(24000 * 2, 0.0f), split(24000 * 2, 0.0f);
        m.Render(whole.data(), 24000, 0, 1.0f);
        m.Render(split.data(), 12000, 0, 1.0f);
        m.Render(split.data() + 12000 * 2, 12000, 12000, 1.0f);
        bool same = true;
        for (size_t i = 0; i < whole.size(); i++)
            if (std::fabs(whole[i] - split[i]) > 1e-5f) same = false;
        CHECK(same);
    }

    // Downbeat (beat 0) tick differs from beat 1 tick (accent frequency).
    {
        std::vector<float> b0(1440 * 2, 0.0f), b1(1440 * 2, 0.0f);
        m.Render(b0.data(), 1440, 0, 1.0f);         // bar downbeat
        m.Render(b1.data(), 1440, 24000, 1.0f);     // beat 1
        bool differ = false;
        for (size_t i = 0; i < b0.size(); i++)
            if (std::fabs(b0[i] - b1[i]) > 1e-3f) differ = true;
        CHECK(differ);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
