// Host tests for SmfIO: write a Standard MIDI File and read it back exactly,
// plus running-status / note-on-zero-velocity parsing.
//
// Build/run:
//   g++ -std=c++17 -Isrc src/model/SmfIO.cpp tests/smfio_tests.cpp -o /tmp/smf
#include "../src/model/SmfIO.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kPath = "/tmp/smfio_test.mid";

int main() {
    // Round-trip: two tracks, several notes, a non-default tempo/PPQ.
    {
        SmfData d;
        d.division = 480;
        d.tempoBpm = 140.0;
        SmfTrack t1; t1.name = "Lead";
        t1.notes.push_back({ 60, 100,   0, 240 });
        t1.notes.push_back({ 64,  80, 240, 240 });
        t1.notes.push_back({ 67, 120, 480, 960 });
        SmfTrack t2; t2.name = "Bass";
        t2.notes.push_back({ 36,  90,   0, 1920 });
        d.tracks = { t1, t2 };
        CHECK(WriteSmf(kPath, d));

        SmfData r;
        CHECK(ReadSmf(kPath, r));
        CHECK(r.division == 480);
        CHECK(r.tracks.size() == 2);
        CHECK((int)(r.tempoBpm + 0.5) == 140);
        CHECK(r.tracks[0].name == "Lead");
        CHECK(r.tracks[0].notes.size() == 3);
        CHECK(r.tracks[0].notes[0].pitch == 60);
        CHECK(r.tracks[0].notes[0].velocity == 100);
        CHECK(r.tracks[0].notes[0].startTick == 0);
        CHECK(r.tracks[0].notes[0].lengthTick == 240);
        CHECK(r.tracks[0].notes[2].startTick == 480);
        CHECK(r.tracks[0].notes[2].lengthTick == 960);
        CHECK(r.tracks[1].name == "Bass");
        CHECK(r.tracks[1].notes.size() == 1);
        CHECK(r.tracks[1].notes[0].pitch == 36);
        CHECK(r.tracks[1].notes[0].lengthTick == 1920);
    }

    // A hand-built track using running status + a note-on with velocity 0 as the
    // note-off. Header: MThd len6 fmt0 ntrks1 div96.
    {
        std::vector<uint8_t> b = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k'
        };
        std::vector<uint8_t> body = {
            0x00, 0x90, 60, 100,   // t=0 note on 60
            0x60, 62, 100,         // t=96 running status: note on 62 (new)
            0x60, 60, 0,           // t=192 running status: 60 vel 0 = off (len 192)
            0x60, 62, 0,           // t=288 62 vel 0 = off (len 192)
            0x00, 0xFF, 0x2F, 0x00 // end of track
        };
        // MTrk length (big-endian).
        b.push_back((uint8_t)(body.size() >> 24));
        b.push_back((uint8_t)(body.size() >> 16));
        b.push_back((uint8_t)(body.size() >> 8));
        b.push_back((uint8_t)body.size());
        b.insert(b.end(), body.begin(), body.end());
        std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
        f.write((const char*)b.data(), (std::streamsize)b.size());
        f.close();

        SmfData r;
        CHECK(ReadSmf(kPath, r));
        CHECK(r.division == 96);
        CHECK(r.tracks.size() == 1);
        CHECK(r.tracks[0].notes.size() == 2);
        // Sorted by startTick: 60 @0 then 62 @96.
        CHECK(r.tracks[0].notes[0].pitch == 60);
        CHECK(r.tracks[0].notes[0].startTick == 0);
        CHECK(r.tracks[0].notes[0].lengthTick == 192);
        CHECK(r.tracks[0].notes[1].pitch == 62);
        CHECK(r.tracks[0].notes[1].startTick == 96);
        CHECK(r.tracks[0].notes[1].lengthTick == 192);
    }

    // A missing file fails cleanly.
    {
        SmfData r;
        CHECK(!ReadSmf("/tmp/does_not_exist_9182.mid", r));
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
