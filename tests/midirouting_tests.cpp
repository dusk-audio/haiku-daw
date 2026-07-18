// Host tests for MidiRouting: which live events reach which track.
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/midirouting_tests.cpp -o /tmp/mr && /tmp/mr
#include "../src/midi/MidiRouting.h"

#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static MidiEvent Ev(uint8_t ch, int32_t source) {
    MidiEvent e = MidiEvent::NoteOn(ch, 60, 100);
    e.source = source;
    return e;
}

int main() {
    // Endpoint filter: only the matching producer gets through.
    {
        CHECK(RouteAccepts(11, 0, Ev(0, 11)));
        CHECK(!RouteAccepts(11, 0, Ev(0, 22)));
    }

    // Endpoint 0 means "any source" — an unrouted track hears everything.
    {
        CHECK(RouteAccepts(0, 0, Ev(0, 11)));
        CHECK(RouteAccepts(0, 0, Ev(0, 22)));
    }

    // An UNTAGGED event (source 0) reaches every route. This is what keeps a
    // source that cannot identify itself (host tests, loopback, anything not
    // coming through the Midi Kit) audible instead of silently filtered out.
    {
        CHECK(RouteAccepts(11, 0, Ev(0, 0)));
        CHECK(RouteAccepts(99, 0, Ev(3, 0)));
    }

    // Channel filter is 1-based; 0 means all channels.
    {
        CHECK(RouteAccepts(0, 1, Ev(0, 11)));    // channel 1 == e.channel 0
        CHECK(!RouteAccepts(0, 2, Ev(0, 11)));
        CHECK(RouteAccepts(0, 2, Ev(1, 11)));
        CHECK(RouteAccepts(0, 16, Ev(15, 11)));
        CHECK(RouteAccepts(0, 0, Ev(7, 11)));    // unfiltered
    }

    // Both filters must pass together: same keyboard, different channels goes
    // to different tracks (a split keyboard / multi-part controller).
    {
        CHECK(RouteAccepts(11, 1, Ev(0, 11)));
        CHECK(!RouteAccepts(11, 1, Ev(1, 11)));   // right endpoint, wrong channel
        CHECK(!RouteAccepts(11, 1, Ev(0, 22)));   // right channel, wrong endpoint
    }

    // Two keyboards, two tracks: each event reaches exactly one of them.
    {
        std::vector<MidiInputRoute> routes = {
            { 1, 11, 0 },
            { 2, 22, 0 },
        };
        const MidiEvent fromA = Ev(0, 11), fromB = Ev(0, 22);
        CHECK(RouteAccepts(RouteFor(routes, 1), fromA));
        CHECK(!RouteAccepts(RouteFor(routes, 1), fromB));
        CHECK(!RouteAccepts(RouteFor(routes, 2), fromA));
        CHECK(RouteAccepts(RouteFor(routes, 2), fromB));
    }

    // A track with no route entry is permissive, not deaf.
    {
        std::vector<MidiInputRoute> routes = { { 1, 11, 0 } };
        const MidiInputRoute r = RouteFor(routes, 7);
        CHECK(r.endpoint == 0 && r.channel == 0);
        CHECK(RouteAccepts(r, Ev(0, 11)));
        CHECK(RouteAccepts(r, Ev(4, 22)));
    }

    std::printf("midirouting_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
