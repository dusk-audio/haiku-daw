// Host tests for SnapGrid: the arrangement's snapping values, their labels and
// the beat-space snap through the tempo map.
//
//   g++ -std=c++17 -Isrc tests/snapgrid_tests.cpp -o /tmp/sg && /tmp/sg

#include "../src/model/SnapGrid.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;
    TempoMap m;
    m.sampleRate = SR;         // 120 BPM -> 24000 frames per beat

    // Off is the identity -- and must not depend on the tempo map at all.
    {
        SnapGrid g{ SnapKind::Off, false };
        CHECK(!g.On());
        CHECK(SnapFrame(m, 12345, g) == 12345);
        CHECK(SnapFrame(m, 0, g) == 0);
        CHECK(std::strcmp(SnapGridLabel(g), "Off") == 0);
    }

    // Note values: steps per beat, then nearest line. A step of 6000 frames
    // (1/16 at 120 BPM/48 kHz).
    {
        SnapGrid g{ SnapKind::Sixteenth, false };
        CHECK(g.On());
        CHECK(SnapFrame(m, 0, g) == 0);
        CHECK(SnapFrame(m, 2999, g) == 0);        // < half a step
        CHECK(SnapFrame(m, 3000, g) == 6000);     // exactly half rounds up
        CHECK(SnapFrame(m, 5999, g) == 6000);
        CHECK(SnapFrame(m, 12000, g) == 12000);
    }
    {
        SnapGrid g{ SnapKind::Quarter, false };   // one step = one beat
        CHECK(SnapFrame(m, 13000, g) == 24000);
        CHECK(SnapFrame(m, 11000, g) == 0);
        CHECK(SnapFrame(m, 24000, g) == 24000);
    }
    {
        SnapGrid g{ SnapKind::Half, false };      // a step is two beats: 48000
        CHECK(SnapFrame(m, 5000, g) == 0);
        CHECK(SnapFrame(m, 20000, g) == 0);       // 0.4167 steps -> 0
        CHECK(SnapFrame(m, 30000, g) == 48000);   // 0.625 steps -> 1
        CHECK(SnapFrame(m, 48000, g) == 48000);
        CHECK(SnapFrame(m, 100000, g) == 96000);  // 2.083 steps -> 2
    }
    {
        SnapGrid g{ SnapKind::Eighth, false };    // step = 12000
        CHECK(SnapFrame(m, 7000, g) == 12000);
        CHECK(SnapFrame(m, 5000, g) == 0);
    }
    {
        SnapGrid g{ SnapKind::ThirtySecond, false };   // step = 3000
        CHECK(SnapFrame(m, 1600, g) == 3000);
        CHECK(SnapFrame(m, 1400, g) == 0);
        CHECK(SnapFrame(m, 3000, g) == 3000);
    }

    // Triplets: 3 steps in the space of 2, so the 1/8 grid's step is 12000/1.5
    // = 8000 frames and the lines fall on the 8th-note triplet positions.
    {
        SnapGrid g{ SnapKind::Eighth, true };
        CHECK(SnapFrame(m, 8000, g) == 8000);
        CHECK(SnapFrame(m, 16000, g) == 16000);
        CHECK(SnapFrame(m, 4000, g) == 8000);     // 0.5 step rounds up
        CHECK(SnapFrame(m, 2000, g) == 0);
        CHECK(SnapFrame(m, 12000, g) == 16000);   // between lines -> the nearer
        CHECK(std::strcmp(SnapGridLabel(g), "1/8T") == 0);
    }

    // A note before the project start must not snap to a negative frame.
    {
        SnapGrid g{ SnapKind::Quarter, false };
        CHECK(SnapFrame(m, -5000, g) >= 0);
        CHECK(SnapFrame(m, -1, g) == 0);
    }

    // Bar snapping is the meter's bar, not four beats: 3/4 bars from a change.
    {
        SnapGrid g{ SnapKind::Bar, false };
        CHECK(SnapFrame(m, 0, g) == 0);
        CHECK(SnapFrame(m, 100000, g) == 96000);   // 4.17 beats -> bar 2
        CHECK(SnapFrame(m, 150000, g) == 192000);  // 6.25 beats -> the nearer bar
    }
    {
        TempoMap w;
        w.sampleRate = SR;
        w.SetMeterAt(0, 3, 4);                          // 3 beats = 72000 frames
        SnapGrid g{ SnapKind::Bar, false };
        CHECK(SnapFrame(w, 1000, g) == 0);
        CHECK(SnapFrame(w, 40000, g) == 72000);          // past the half bar
        CHECK(SnapFrame(w, 70000, g) == 72000);
        CHECK(SnapFrame(w, 100000, g) == 72000);
        CHECK(SnapFrame(w, 110000, g) == 144000);
    }

    // A tempo change moves the grid with the music: 120 BPM for 4 beats, then
    // 240 BPM (12000 frames/beat). Beat 5 starts at 108000 frames.
    {
        TempoMap v;
        v.sampleRate = SR;
        v.SetTempoAt(96000, 240.0);
        SnapGrid g{ SnapKind::Quarter, false };
        CHECK(SnapFrame(v, 108000, g) == 108000);        // beat 5 exactly
        CHECK(SnapFrame(v, 113000, g) == 108000);        // nearer beat 5
        CHECK(SnapFrame(v, 115000, g) == 120000);        // nearer beat 6
    }

    // The labels: every kind has one, and the triplet label carries the T.
    {
        for (int i = 0; i <= (int)SnapKind::ThirtySecond; i++) {
            const SnapGrid g{ (SnapKind)i, false };
            const char* l = SnapGridLabel(g);
            CHECK(l != nullptr && l[0] != '\0');
        }
        CHECK(std::strcmp(SnapGridLabel({ SnapKind::Bar, false }), "Bar") == 0);
        CHECK(std::strcmp(SnapGridLabel({ SnapKind::Sixteenth, true }), "1/16T") == 0);
        CHECK(std::strcmp(SnapGridLabel({ SnapKind::ThirtySecond, true }), "1/32T") == 0);
    }

    // The menu's division list is the plan's order and matches the labels.
    {
        int n = 0;
        const SnapDivision* d = SnapDivisions(&n);
        CHECK(n == 6);
        const SnapKind want[6] = { SnapKind::Bar, SnapKind::Half, SnapKind::Quarter,
                                   SnapKind::Eighth, SnapKind::Sixteenth,
                                   SnapKind::ThirtySecond };
        for (int i = 0; i < n && i < 6; i++) {
            CHECK(d[i].kind == want[i]);
            CHECK(std::strcmp(d[i].label,
                              SnapGridLabel({ d[i].kind, false })) == 0);
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
