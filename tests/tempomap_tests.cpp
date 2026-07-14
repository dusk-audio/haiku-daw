// Host tests for TempoMap: frame<->beat conversion, meter, BBT across changes.
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/tempomap_tests.cpp -o /tmp/tm && /tmp/tm
#include "../src/model/TempoMap.h"

#include <cmath>
#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;

    // Constant 120 BPM: 1 beat = 24000 frames.
    {
        TempoMap m; m.sampleRate = SR;
        CHECK(std::fabs(m.FramesPerBeatAt(0) - 24000.0) < 1e-6);
        CHECK(std::fabs(m.BeatAt(24000) - 1.0) < 1e-6);
        CHECK(std::fabs(m.BeatAt(96000) - 4.0) < 1e-6);
        CHECK(m.FrameAt(4.0) == 96000);
        int bar = 0, beat = 0;
        m.BarBeat(96000, &bar, &beat);       // beat 4 -> bar 2 beat 1
        CHECK(bar == 2 && beat == 1);
        m.BarBeat(48000, &bar, &beat);       // beat 2 -> bar 1 beat 3
        CHECK(bar == 1 && beat == 3);
    }

    // Tempo change: 120 BPM for the first 96000 frames (4 beats), then 240 BPM
    // (1 beat = 12000 frames).
    {
        TempoMap m; m.sampleRate = SR;
        m.SetTempoAt(96000, 240.0);
        CHECK(std::fabs(m.BpmAt(0) - 120.0) < 1e-9);
        CHECK(std::fabs(m.BpmAt(96000) - 240.0) < 1e-9);
        CHECK(std::fabs(m.BeatAt(96000) - 4.0) < 1e-6);
        // 96000 + 12000 = 108000 -> 5 beats.
        CHECK(std::fabs(m.BeatAt(108000) - 5.0) < 1e-6);
        CHECK(m.FrameAt(5.0) == 108000);
        // Round trip a few positions.
        for (Frame f : { (Frame)0, (Frame)50000, (Frame)96000, (Frame)150000 }) {
            const double b = m.BeatAt(f);
            CHECK(std::llabs((long long)(m.FrameAt(b) - f)) <= 1);
        }
    }

    // Meter change: 4/4 for two bars (8 beats @ 120 = 192000 frames), then 3/4.
    {
        TempoMap m; m.sampleRate = SR;               // 24000 frames/beat
        m.SetMeterAt(192000, 3, 4);                  // bar 3 onward is 3/4
        int num = 0, den = 0;
        m.Meter(0, &num, &den);      CHECK(num == 4 && den == 4);
        m.Meter(192000, &num, &den); CHECK(num == 3 && den == 4);
        int bar = 0, beat = 0;
        m.BarBeat(0, &bar, &beat);        CHECK(bar == 1 && beat == 1);
        m.BarBeat(192000, &bar, &beat);   CHECK(bar == 3 && beat == 1);
        // One 3/4 bar later: 192000 + 3*24000 = 264000 -> bar 4 beat 1.
        m.BarBeat(264000, &bar, &beat);   CHECK(bar == 4 && beat == 1);
        // Mid 3/4 bar: +2 beats -> bar 3 beat 3.
        m.BarBeat(192000 + 48000, &bar, &beat); CHECK(bar == 3 && beat == 3);
    }

    // Remove restores; frame-0 anchor is protected.
    {
        TempoMap m; m.sampleRate = SR;
        m.SetTempoAt(48000, 90.0);
        CHECK(m.Tempos().size() == 2);
        m.RemoveTempoAt(48000);
        CHECK(m.Tempos().size() == 1);
        m.RemoveTempoAt(0);                 // ignored
        CHECK(m.Tempos().size() == 1);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
