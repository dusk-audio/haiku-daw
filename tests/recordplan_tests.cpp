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

        // A capture that is `offset` frames late starts with the tail of the
        // pass before the loop: those frames are dropped, and every take keeps
        // its place on the timeline.
        auto off = LoopTakes(0, 1000, 2500, 250);
        CHECK(off.size() == 3);
        CHECK(off[0].sourceOffset == 250  && off[0].lengthFrames == 1000);
        CHECK(off[1].sourceOffset == 1250 && off[1].lengthFrames == 1000);
        CHECK(off[2].sourceOffset == 2250 && off[2].lengthFrames == 250);
        for (auto& tk : off) CHECK(tk.startFrame == 0);
        // An offset that eats the whole capture leaves no take at all.
        CHECK(LoopTakes(0, 1000, 250, 250).empty());
        CHECK(LoopTakes(0, 1000, 100, 250).empty());
        // And a negative offset is treated as none (out-of-range input).
        CHECK(LoopTakes(0, 1000, 2000, -5).size() == 2);
    }

    // Device latency -> frames: what the Media Kit reports, in timeline frames.
    {
        CHECK(LatencyUsToFrames(0, 48000.0) == 0);
        CHECK(LatencyUsToFrames(10000, 48000.0) == 480);
        CHECK(LatencyUsToFrames(1000, 44100.0) == 44);      // rounds to nearest
        CHECK(LatencyUsToFrames(-100, 48000.0) == 0);       // nonsense in, 0 out
        CHECK(LatencyUsToFrames(10000, 0.0) == 0);
        CHECK(LatencyUsToFrames(10000, -48000.0) == 0);
    }

    // Round-trip compensation: a take slides earlier by the round-trip latency.
    {
        // Normal case: recStart 10000, 5000 captured, 480-frame round trip.
        TakeRegion r = CompensateRoundTrip(10000, 5000, 480);
        CHECK(r.startFrame   == 10000 - 480);   // slid earlier
        CHECK(r.sourceOffset == 0);             // whole capture kept
        CHECK(r.lengthFrames == 5000);
        // Zero / negative latency -> unchanged.
        r = CompensateRoundTrip(10000, 5000, 0);
        CHECK(r.startFrame == 10000 && r.sourceOffset == 0 && r.lengthFrames == 5000);
        r = CompensateRoundTrip(10000, 5000, -100);
        CHECK(r.startFrame == 10000 && r.sourceOffset == 0);
        // Near the timeline start: shifting earlier would go negative, so the
        // lead is dropped from the source and the start clamps to 0.
        r = CompensateRoundTrip(300, 5000, 480);
        CHECK(r.startFrame   == 0);
        CHECK(r.sourceOffset == 180);           // 480 - 300 dropped from the head
        CHECK(r.lengthFrames == 5000 - 180);
        // Round trip longer than the whole capture -> empty (nothing survives).
        r = CompensateRoundTrip(0, 100, 480);
        CHECK(r.startFrame == 0 && r.sourceOffset == 480 && r.lengthFrames == 0);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
