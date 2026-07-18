// Host tests for MidiControl: step-function CC / pitch-bend evaluation and the
// CC7 x CC11 channel-gain the synth renders through.

#include "../src/model/MidiControl.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static MidiClipEvent CC(int cc, Frame at, int v) {
    return { MidiClipEvent::CC, at, cc, v };
}

int main() {
    // Empty -> defaults.
    {
        std::vector<MidiClipEvent> ev;
        CHECK(CcValueAt(ev, 7, 1000, 127) == 127);
        CHECK(PitchBendAt(ev, 1000) == 8192);
        CHECK(std::fabs(MidiChannelGain(ev, 1000) - 1.0f) < 1e-6f);
    }

    // Step function: latest event at/before the frame wins; before the first
    // event the default holds.
    {
        std::vector<MidiClipEvent> ev = {
            CC(7, 100, 40), CC(7, 500, 100), CC(7, 900, 0) };
        CHECK(CcValueAt(ev, 7,   0, 127) == 127);   // before first -> default
        CHECK(CcValueAt(ev, 7,  99, 127) == 127);
        CHECK(CcValueAt(ev, 7, 100, 127) == 40);    // at the event
        CHECK(CcValueAt(ev, 7, 300, 127) == 40);    // between
        CHECK(CcValueAt(ev, 7, 500, 127) == 100);
        CHECK(CcValueAt(ev, 7, 899, 127) == 100);
        CHECK(CcValueAt(ev, 7, 900, 127) == 0);     // last
        CHECK(CcValueAt(ev, 7, 9999, 127) == 0);    // after last -> holds
    }

    // Unsorted events resolve to the latest-by-frame, not last-in-vector.
    {
        std::vector<MidiClipEvent> ev = {
            CC(7, 900, 0), CC(7, 100, 40), CC(7, 500, 100) };
        CHECK(CcValueAt(ev, 7, 600, 127) == 100);
        CHECK(CcValueAt(ev, 7, 950, 127) == 0);
    }

    // Different controllers don't cross-talk.
    {
        std::vector<MidiClipEvent> ev = { CC(7, 100, 30), CC(11, 100, 64) };
        CHECK(CcValueAt(ev, 7,  200, 127) == 30);
        CHECK(CcValueAt(ev, 11, 200, 127) == 64);
        CHECK(CcValueAt(ev, 10, 200, 127) == 127);   // no CC10 -> default
    }

    // Channel gain = CC7 x CC11, each normalized; a zero on either mutes.
    {
        std::vector<MidiClipEvent> ev = { CC(7, 0, 127), CC(11, 0, 127) };
        CHECK(std::fabs(MidiChannelGain(ev, 10) - 1.0f) < 1e-6f);
        ev.push_back(CC(7, 100, 0));                  // volume to 0
        CHECK(MidiChannelGain(ev, 200) < 1e-6f);      // muted
        ev.push_back(CC(7, 300, 64));                 // ~half
        ev.push_back(CC(11, 300, 127));
        CHECK(std::fabs(MidiChannelGain(ev, 400) - 64.0f / 127.0f) < 1e-4f);
    }

    // Pitch bend step function (14-bit, center default).
    {
        std::vector<MidiClipEvent> ev = {
            { MidiClipEvent::PitchBend, 200, 0, 12000 },
            { MidiClipEvent::PitchBend, 600, 0, 4000 } };
        CHECK(PitchBendAt(ev, 0)   == 8192);
        CHECK(PitchBendAt(ev, 200) == 12000);
        CHECK(PitchBendAt(ev, 599) == 12000);
        CHECK(PitchBendAt(ev, 600) == 4000);
    }

    // CC10 pan: absent/64 = centered; 0 = hard left, 127 = hard right.
    {
        std::vector<MidiClipEvent> ev;
        CHECK(std::fabs(MidiChannelPan(ev, 100) - 0.0f) < 1e-6f);   // absent -> center
        ev.push_back(CC(10, 100, 64));
        CHECK(std::fabs(MidiChannelPan(ev, 200) - 0.0f) < 1e-6f);
        ev.push_back(CC(10, 300, 0));
        CHECK(std::fabs(MidiChannelPan(ev, 400) - (-1.0f)) < 1e-4f); // hard left
        ev.push_back(CC(10, 500, 127));
        CHECK(std::fabs(MidiChannelPan(ev, 600) - 1.0f) < 1e-4f);    // hard right (clamped)
    }

    // Channel gains: with no CC10 the pan is unity so gL == gR == MidiChannelGain
    // (existing projects are bit-identical); CC10 attenuates only the far side.
    {
        std::vector<MidiClipEvent> ev = { CC(7, 0, 127), CC(11, 0, 127) };
        float l = 0.0f, r = 0.0f;
        MidiChannelGains(ev, 10, &l, &r);
        CHECK(std::fabs(l - 1.0f) < 1e-6f);          // centered -> untouched
        CHECK(std::fabs(r - 1.0f) < 1e-6f);

        ev.push_back(CC(10, 100, 0));                // hard left
        MidiChannelGains(ev, 200, &l, &r);
        CHECK(std::fabs(l - 1.0f) < 1e-4f);          // near side keeps unity
        CHECK(r < 1e-4f);                            // far side silent

        ev.push_back(CC(10, 300, 127));              // hard right
        MidiChannelGains(ev, 400, &l, &r);
        CHECK(l < 1e-4f);
        CHECK(std::fabs(r - 1.0f) < 1e-4f);

        // Pan scales the CC7 x CC11 magnitude, it doesn't replace it.
        ev.push_back(CC(7,  500, 64));               // ~half volume
        ev.push_back(CC(10, 500, 64));               // back to center
        MidiChannelGains(ev, 600, &l, &r);
        CHECK(std::fabs(l - 64.0f / 127.0f) < 1e-4f);
        CHECK(std::fabs(r - 64.0f / 127.0f) < 1e-4f);
    }

    std::printf("midicontrol_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
