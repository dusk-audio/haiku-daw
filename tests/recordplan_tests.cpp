// Host tests for RecordPlan: count-in frames, punch trimming, loop takes.
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/recordplan_tests.cpp -o /tmp/rp && /tmp/rp
#include "../src/model/RecordPlan.h"

#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;

    // Count-in: 120 BPM, 4/4 -> 1 bar = 4 beats = 96000 frames.
    {
        TempoMap tm; tm.sampleRate = SR;
        CHECK(CountInFrames(tm, 0, 0) == 0);
        CHECK(CountInFrames(tm, 0, 1) == 96000);
        CHECK(CountInFrames(tm, 0, 2) == 192000);
        // From a non-zero playhead, still one bar forward.
        CHECK(CountInFrames(tm, 24000, 1) == 96000);
        // 3/4 meter -> 1 bar = 3 beats = 72000.
        tm.SetMeterAt(0, 3, 4);
        CHECK(CountInFrames(tm, 0, 1) == 72000);
    }

    // Punch: capture [1000, 1000+5000) trimmed to [2000, 4000).
    {
        TakeRegion r;
        CHECK(PunchedTake(1000, 5000, 2000, 4000, &r));
        CHECK(r.startFrame == 2000);
        CHECK(r.sourceOffset == 1000);   // 2000 - 1000
        CHECK(r.lengthFrames == 2000);   // 4000 - 2000
        // Punch entirely before the capture -> no region.
        CHECK(!PunchedTake(1000, 5000, 0, 500, &r));
        // Degenerate punch -> no region.
        CHECK(!PunchedTake(1000, 5000, 3000, 3000, &r));
        // Punch wider than capture clamps to the capture bounds.
        CHECK(PunchedTake(1000, 5000, 0, 100000, &r));
        CHECK(r.startFrame == 1000 && r.lengthFrames == 5000);
    }

    // Loop takes: loop [0,1000), captured 2500 -> 3 takes (1000,1000,500).
    {
        auto ts = LoopTakes(0, 1000, 2500);
        CHECK(ts.size() == 3);
        CHECK(ts[0].sourceOffset == 0    && ts[0].lengthFrames == 1000);
        CHECK(ts[1].sourceOffset == 1000 && ts[1].lengthFrames == 1000);
        CHECK(ts[2].sourceOffset == 2000 && ts[2].lengthFrames == 500);
        for (auto& t : ts) CHECK(t.startFrame == 0);
        // Exact multiple -> whole passes only.
        CHECK(LoopTakes(0, 1000, 2000).size() == 2);
        // Degenerate loop -> none.
        CHECK(LoopTakes(500, 500, 2000).empty());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
