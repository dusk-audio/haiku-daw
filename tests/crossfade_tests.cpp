// Host tests for ComputeCrossfades: overlap -> auto crossfade fades.
//
// Build/run:
//   g++ -std=c++17 -Isrc src/model/*.cpp tests/crossfade_tests.cpp -o /tmp/xf
#include "../src/model/Crossfade.h"

#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static Clip mk(Frame start, Frame len, Frame fin = 0, Frame fout = 0) {
    Clip c; c.startFrame = start; c.lengthFrames = len;
    c.fadeInFrames = fin; c.fadeOutFrames = fout; c.sourcePath = "x.wav";
    return c;
}

int main() {
    // No overlap: fades unchanged.
    {
        std::vector<Clip> cs = { mk(0, 1000, 10, 20), mk(1000, 1000) };
        auto f = ComputeCrossfades(cs);
        CHECK(f.size() == 2);
        CHECK(f[0].fadeIn == 10 && f[0].fadeOut == 20);
        CHECK(f[1].fadeIn == 0 && f[1].fadeOut == 0);
    }

    // Overlap of 300: earlier fades out 300, later fades in 300.
    {
        std::vector<Clip> cs = { mk(0, 1000), mk(700, 1000) };
        auto f = ComputeCrossfades(cs);
        CHECK(f[0].fadeOut == 300);
        CHECK(f[1].fadeIn == 300);
        CHECK(f[0].fadeIn == 0 && f[1].fadeOut == 0);
    }

    // Existing longer user fade wins over a smaller overlap (max-combine).
    {
        std::vector<Clip> cs = { mk(0, 1000, 0, 500), mk(800, 1000) };
        auto f = ComputeCrossfades(cs);
        CHECK(f[0].fadeOut == 500);      // 500 > overlap 200
        CHECK(f[1].fadeIn == 200);
    }

    // Overlap clamped to the shorter clip's length.
    {
        std::vector<Clip> cs = { mk(0, 1000), mk(200, 100) };  // b len 100
        auto f = ComputeCrossfades(cs);
        CHECK(f[0].fadeOut == 100);      // clamped to b.len
        CHECK(f[1].fadeIn == 100);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
