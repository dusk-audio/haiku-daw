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

    // CrossfadeOverlap is the shared rule: the renderers derive fades from it and
    // the timeline draws the overlap band from it. Pin it directly so the two can
    // never disagree about where a crossfade is or how long it lasts.
    {
        CHECK(CrossfadeOverlap(mk(0, 1000), mk(1000, 1000)) == 0);   // butting
        CHECK(CrossfadeOverlap(mk(0, 1000), mk(1200, 500)) == 0);    // a gap
        CHECK(CrossfadeOverlap(mk(0, 1000), mk(700, 1000)) == 300);
        CHECK(CrossfadeOverlap(mk(0, 1000), mk(200, 100)) == 100);   // short b
        // A user fade does not widen the overlap (only the effective fade).
        CHECK(CrossfadeOverlap(mk(0, 1000, 0, 500), mk(800, 1000)) == 200);
        // Stacked loop-record takes are alternatives, never crossfade partners.
        {
            Clip a = mk(0, 1000), b = mk(700, 1000);
            a.takeGroup = 1; b.takeGroup = 1;
            CHECK(CrossfadeOverlap(a, b) == 0);
        }
        // The overlap the timeline shades must equal the later clip's auto
        // fade-in, so the band and the ramp always line up.
        {
            std::vector<Clip> cs = { mk(0, 1000), mk(600, 1000) };
            auto f = ComputeCrossfades(cs);
            CHECK(CrossfadeOverlap(cs[0], cs[1]) == f[1].fadeIn);
            CHECK(CrossfadeOverlap(cs[0], cs[1]) == f[0].fadeOut);
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
