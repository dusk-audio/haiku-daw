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

    // Channel-event round-trip: CC, pitch bend, program change, aftertouch on a
    // track alongside notes, written and read back exactly.
    {
        SmfData d;
        d.division = 480; d.tempoBpm = 120.0;
        SmfTrack t; t.name = "Ctrl";
        t.notes.push_back({ 60, 100, 0, 480 });
        t.events.push_back({ SmfEvent::CC,              0,   1, 64 });  // mod
        t.events.push_back({ SmfEvent::CC,            240,   7, 100 }); // volume
        t.events.push_back({ SmfEvent::PitchBend,     120,   0, 10000 });
        t.events.push_back({ SmfEvent::Program,         0,   0, 5 });
        t.events.push_back({ SmfEvent::ChannelPressure, 360, 0, 77 });
        d.tracks = { t };
        CHECK(WriteSmf(kPath, d));

        SmfData r;
        CHECK(ReadSmf(kPath, r));
        CHECK(r.tracks.size() == 1);
        CHECK(r.tracks[0].notes.size() == 1);
        CHECK(r.tracks[0].events.size() == 5);
        // Sorted by tick: PC@0, CC1@0, PB@120, CC7@240, pressure@360.
        auto& ev = r.tracks[0].events;
        // Find each by (type,data) since same-tick order isn't guaranteed.
        int nCC = 0, nPB = 0, nProg = 0, nPress = 0;
        for (const SmfEvent& e : ev) {
            if (e.type == SmfEvent::CC && e.data == 1)  { CHECK(e.value == 64);  CHECK(e.tick == 0);   nCC++; }
            if (e.type == SmfEvent::CC && e.data == 7)  { CHECK(e.value == 100); CHECK(e.tick == 240); nCC++; }
            if (e.type == SmfEvent::PitchBend)          { CHECK(e.value == 10000); CHECK(e.tick == 120); nPB++; }
            if (e.type == SmfEvent::Program)            { CHECK(e.value == 5);   CHECK(e.tick == 0);   nProg++; }
            if (e.type == SmfEvent::ChannelPressure)    { CHECK(e.value == 77);  CHECK(e.tick == 360); nPress++; }
        }
        CHECK(nCC == 2 && nPB == 1 && nProg == 1 && nPress == 1);
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

    // Malformed input must not crash/OOB and must not emit a partial track.
    auto writeRaw = [](const std::vector<uint8_t>& b) {
        std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
        f.write((const char*)b.data(), (std::streamsize)b.size());
    };
    // A declared MTrk length that runs past EOF is a truncated file: rejected.
    {
        std::vector<uint8_t> b = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k', 0,0,0x03,0xE8,   // tlen = 1000, but only 4 body bytes
            0x00, 0xFF, 0x2F, 0x00
        };
        writeRaw(b);
        SmfData r;
        CHECK(!ReadSmf(kPath, r));         // truncated track -> no output
        CHECK(r.tracks.empty());
    }
    // A meta event whose length overruns the track bound is rejected (would
    // otherwise read into the next track / past the track).
    {
        // tlen = 6; body: delta0, FF 03 (text) len=5, but only 2 bytes remain.
        std::vector<uint8_t> b = {
            'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
            'M','T','r','k', 0,0,0,6,
            0x00, 0xFF, 0x03, 0x05, 'A', 'B'
        };
        writeRaw(b);
        SmfData r;
        CHECK(!ReadSmf(kPath, r));         // overrunning meta -> track rejected
        CHECK(r.tracks.empty());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
