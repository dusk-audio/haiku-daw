// Host-buildable tests for musical grid math: frames-per-beat/bar, snapping to
// the nearest division, and bar/beat readout.

#include "../src/model/Grid.h"

#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    Grid g; g.sampleRate = 48000.0; g.tempoBPM = 120.0; g.beatsPerBar = 4;

    // 120 BPM @ 48k: 0.5 s/beat = 24000 frames/beat, 96000 frames/bar.
    CHECK(g.FramesPerBeat() == 24000.0);
    CHECK(g.FramesPerBar() == 96000.0);

    // Snap to beats (division 1).
    CHECK(g.Snap(0, 1) == 0);
    CHECK(g.Snap(1000, 1) == 0);       // nearest beat is 0
    CHECK(g.Snap(13000, 1) == 24000);  // 0.54 beat -> beat 1
    CHECK(g.Snap(24000, 1) == 24000);
    CHECK(g.Snap(37000, 1) == 48000);  // 1.54 -> beat 2

    // Snap to sixteenths (division 4): step = 6000 frames.
    CHECK(g.Snap(5000, 4) == 6000);
    CHECK(g.Snap(2000, 4) == 0);

    // Bar/beat readout (1-indexed).
    int bar, beat;
    g.BarBeat(0, &bar, &beat);       CHECK(bar == 1 && beat == 1);
    g.BarBeat(24000, &bar, &beat);   CHECK(bar == 1 && beat == 2);
    g.BarBeat(96000, &bar, &beat);   CHECK(bar == 2 && beat == 1);
    g.BarBeat(96000 + 48000, &bar, &beat); CHECK(bar == 2 && beat == 3);

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
