// Host tests for FrameDelay: the RT PDC stereo delay line. Verifies exact
// integer-sample delay, cross-block continuity, the zero-delay fast path, and
// level scaling.

#include "../src/engine/FrameDelay.h"

#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    // d == 0: plain accumulate (adds, does not overwrite).
    {
        FrameDelay fd; fd.Prepare(0);
        std::vector<float> in = {1, 2, 3, 4};      // 2 stereo frames
        std::vector<float> dst = {10, 20, 30, 40};
        fd.ProcessAdd(in.data(), dst.data(), 2, 1.0f);
        CHECK(dst[0] == 11 && dst[1] == 22 && dst[2] == 33 && dst[3] == 44);
    }

    // d == 3: an impulse at input frame 0 emerges at output frame 3.
    {
        FrameDelay fd; fd.Prepare(3);
        std::vector<float> in(8 * 2, 0.0f);
        in[0] = 1.0f; in[1] = 1.0f;                // impulse at frame 0
        std::vector<float> dst(8 * 2, 0.0f);
        fd.ProcessAdd(in.data(), dst.data(), 8, 1.0f);
        CHECK(dst[0] == 0 && dst[1] == 0);         // frames 0..2 silent
        CHECK(dst[2 * 2] == 0);
        CHECK(dst[3 * 2 + 0] == 1.0f && dst[3 * 2 + 1] == 1.0f);   // impulse at 3
        CHECK(dst[4 * 2 + 0] == 0.0f);
    }

    // Cross-block continuity: a delay of 5 with a 2-frame impulse split across
    // 3-frame blocks. Impulse at global frame 1 must appear at global frame 6.
    {
        FrameDelay fd; fd.Prepare(5);
        // Block 1: frames 0..2, impulse at frame 1.
        std::vector<float> b1(3 * 2, 0.0f);
        b1[1 * 2 + 0] = 1.0f; b1[1 * 2 + 1] = 1.0f;
        std::vector<float> o1(3 * 2, 0.0f);
        fd.ProcessAdd(b1.data(), o1.data(), 3, 1.0f);
        for (float v : o1) CHECK(v == 0.0f);       // nothing out yet (delay 5)
        // Block 2: frames 3..5, silent.
        std::vector<float> b2(3 * 2, 0.0f), o2(3 * 2, 0.0f);
        fd.ProcessAdd(b2.data(), o2.data(), 3, 1.0f);
        for (float v : o2) CHECK(v == 0.0f);       // global 3..5 still silent
        // Block 3: frames 6..8, silent input — the delayed impulse lands at 6.
        std::vector<float> b3(3 * 2, 0.0f), o3(3 * 2, 0.0f);
        fd.ProcessAdd(b3.data(), o3.data(), 3, 1.0f);
        CHECK(o3[0 * 2 + 0] == 1.0f && o3[0 * 2 + 1] == 1.0f);   // global frame 6
        CHECK(o3[1 * 2 + 0] == 0.0f);
    }

    // Level scaling.
    {
        FrameDelay fd; fd.Prepare(1);
        std::vector<float> in(2 * 2, 0.0f); in[0] = 2.0f; in[1] = 4.0f;
        std::vector<float> dst(2 * 2, 0.0f);
        fd.ProcessAdd(in.data(), dst.data(), 2, 0.5f);
        CHECK(dst[1 * 2 + 0] == 1.0f && dst[1 * 2 + 1] == 2.0f);   // delayed *0.5
    }

    // Reset clears the tail.
    {
        FrameDelay fd; fd.Prepare(2);
        std::vector<float> in(2 * 2, 1.0f), dst(2 * 2, 0.0f);
        fd.ProcessAdd(in.data(), dst.data(), 2, 1.0f);   // load the ring
        fd.Reset();
        std::vector<float> dst2(2 * 2, 0.0f);
        std::vector<float> zero(2 * 2, 0.0f);
        fd.ProcessAdd(zero.data(), dst2.data(), 2, 1.0f);
        for (float v : dst2) CHECK(v == 0.0f);           // no stale tail
    }

    std::printf("framedelay_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
