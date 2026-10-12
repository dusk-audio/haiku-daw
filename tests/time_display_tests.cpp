// Host-buildable tests for the transport's four time formats and their inverse
// (plan M3.1): what the big display shows, what a typed position means, and
// that the two agree — including under a meter change, where the BBT walk and
// TempoMap::BarBeat have to be each other's inverse.

#include "../src/model/TimeDisplay.h"

#include <cstdio>
#include <string>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static bool ParseEq(TimeMode m, const char* text, double sr, const TempoMap& map,
                    Frame want) {
    Frame got = -1;
    if (!ParseTimePosition(m, text, sr, map, &got)) return false;
    return got == want;
}

int main() {
    const double SR = 48000.0;
    TempoMap map;
    map.sampleRate = SR;          // 120 BPM, 4/4 at frame 0
    // 0.5 s/beat -> 24000 frames/beat, 96000 frames/bar at 48 kHz.

    // --- formatting ------------------------------------------------------
    CHECK(FormatTimePosition(TimeMode::Bbt, 0, SR, map) == "1.1.000");
    CHECK(FormatTimePosition(TimeMode::Bbt, 24000, SR, map) == "1.2.000");
    CHECK(FormatTimePosition(TimeMode::Bbt, 96000, SR, map) == "2.1.000");
    CHECK(FormatTimePosition(TimeMode::Bbt, 12000, SR, map) == "1.1.240");
    CHECK(FormatTimePosition(TimeMode::MinSec, 0, SR, map) == "0:00.000");
    CHECK(FormatTimePosition(TimeMode::MinSec, 72000, SR, map) == "0:01.500");
    // 90 s == 1:30.000 (60 s + 30 s at 48 kHz).
    CHECK(FormatTimePosition(TimeMode::MinSec, (Frame)(90 * SR), SR, map)
          == "1:30.000");
    CHECK(FormatTimePosition(TimeMode::MinSec, 48001, SR, map) == "0:01.000");
    CHECK(FormatTimePosition(TimeMode::Samples, 1234567, SR, map) == "1234567");
    CHECK(FormatTimePosition(TimeMode::Timecode, 0, SR, map) == "00:00:00:00");
    // 1 h 2 min 3 s + 15 frames at 30 fps.
    {
        const Frame f = (Frame)((3600 + 120 + 3 + 15.0 / 30.0) * SR);
        CHECK(FormatTimePosition(TimeMode::Timecode, f, SR, map) == "01:02:03:15");
    }
    // A negative frame reads as the start, never as a negative time.
    CHECK(FormatTimePosition(TimeMode::MinSec, -500, SR, map) == "0:00.000");

    // --- the mode cycle --------------------------------------------------
    CHECK(NextTimeMode(TimeMode::Bbt)      == TimeMode::MinSec);
    CHECK(NextTimeMode(TimeMode::MinSec)   == TimeMode::Samples);
    CHECK(NextTimeMode(TimeMode::Samples)  == TimeMode::Timecode);
    CHECK(NextTimeMode(TimeMode::Timecode) == TimeMode::Bbt);
    CHECK(std::string(TimeModeTag(TimeMode::Bbt)) == "BBT");
    CHECK(std::string(TimeModeTag(TimeMode::Timecode)) == "TC");

    // --- BBT parse/format round trips ------------------------------------
    CHECK(ParseEq(TimeMode::Bbt, "1.1.000", SR, map, 0));
    CHECK(ParseEq(TimeMode::Bbt, "1.2",     SR, map, 24000));
    CHECK(ParseEq(TimeMode::Bbt, "2.1.000", SR, map, 96000));
    CHECK(ParseEq(TimeMode::Bbt, "1.1.240", SR, map, 12000));
    CHECK(ParseEq(TimeMode::Bbt, "1",       SR, map, 0));       // bar only
    CHECK(ParseEq(TimeMode::Bbt, "3",       SR, map, 192000));
    // A fractional beat field is beats + ticks.
    CHECK(ParseEq(TimeMode::Bbt, "1.1.5",   SR, map, 250));
    // Round trip: every beat of the first two bars formats back to itself.
    for (int f = 0; f < 96000; f += 24000 / 4)
        CHECK(ParseEq(TimeMode::Bbt,
                      FormatTimePosition(TimeMode::Bbt, f, SR, map).c_str(),
                      SR, map, f));

    // --- min:sec ---------------------------------------------------------
    CHECK(ParseEq(TimeMode::MinSec, "0:00.000", SR, map, 0));
    CHECK(ParseEq(TimeMode::MinSec, "1:30",     SR, map, (Frame)(90 * SR)));
    CHECK(ParseEq(TimeMode::MinSec, "1:30.5",   SR, map, (Frame)(90.5 * SR)));
    CHECK(ParseEq(TimeMode::MinSec, "2",        SR, map, 96000));   // seconds
    CHECK(ParseEq(TimeMode::MinSec, "0.5",      SR, map, 24000));
    CHECK(!ParseEq(TimeMode::MinSec, "1:2:3",   SR, map, 0));   // too many fields

    // --- samples ---------------------------------------------------------
    CHECK(ParseEq(TimeMode::Samples, "0",       SR, map, 0));
    CHECK(ParseEq(TimeMode::Samples, "12345",   SR, map, 12345));
    CHECK(ParseEq(TimeMode::Samples, " 480000", SR, map, 480000));

    // --- timecode --------------------------------------------------------
    CHECK(ParseEq(TimeMode::Timecode, "00:00:00:00", SR, map, 0));
    CHECK(ParseEq(TimeMode::Timecode, "00:00:01:00", SR, map, 48000));
    CHECK(ParseEq(TimeMode::Timecode, "00:00:01:15", SR, map, 48000 + 24000));
    CHECK(ParseEq(TimeMode::Timecode, "01:02:03:15", SR, map,
                  (Frame)((3600 + 120 + 3 + 0.5) * SR)));
    CHECK(ParseEq(TimeMode::Timecode, "01:30", SR, map, 90 * 48000));   // mm:ss

    // --- what a typed position must NOT do -------------------------------
    Frame out = 12345;
    CHECK(!ParseTimePosition(TimeMode::Samples, "", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::Samples, "12a", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::Samples, "-5", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::MinSec, "banana", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::Bbt, "0.1", SR, map, &out));      // bar 0
    CHECK(!ParseTimePosition(TimeMode::Bbt, "1.0", SR, map, &out));      // beat 0
    CHECK(!ParseTimePosition(TimeMode::Bbt, "1.9", SR, map, &out));      // no beat 9
    CHECK(!ParseTimePosition(TimeMode::Timecode, "00:99:00:00", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::Timecode, "00:00:00:30", SR, map, &out));
    CHECK(!ParseTimePosition(TimeMode::Timecode, "12", SR, map, &out));
    CHECK(out == 12345);   // a refused parse leaves the caller's frame alone
    // No sample rate: nothing can be converted, so nothing is accepted.
    CHECK(!ParseTimePosition(TimeMode::MinSec, "1:00", 0.0, map, &out));

    // --- a meter change: BBT's two directions must agree ------------------
    map.SetMeterAt(96000, 3, 4);        // bar 2 becomes 3/4
    CHECK(map.Meters().size() == 2);
    CHECK(FormatTimePosition(TimeMode::Bbt, 0, SR, map) == "1.1.000");
    CHECK(FormatTimePosition(TimeMode::Bbt, 96000, SR, map) == "2.1.000");
    // Bar 3 starts one 3/4 bar after bar 2: 3 beats = 72000 frames.
    CHECK(FormatTimePosition(TimeMode::Bbt, 168000, SR, map) == "3.1.000");
    CHECK(ParseEq(TimeMode::Bbt, "2.1.000", SR, map, 96000));
    CHECK(ParseEq(TimeMode::Bbt, "3.1.000", SR, map, 168000));
    CHECK(ParseEq(TimeMode::Bbt, "2.3.000", SR, map, 96000 + 2 * 24000));
    CHECK(!ParseEq(TimeMode::Bbt, "3.4.000", SR, map, 0));   // 3/4 has no beat 4
    // Round trip through the meter change, bar by bar.
    for (int bar = 1; bar <= 5; bar++) {
        char t[16];
        std::snprintf(t, sizeof(t), "%d.1.000", bar);
        Frame want = -1;
        CHECK(ParseTimePosition(TimeMode::Bbt, t, SR, map, &want));
        if (want >= 0)
            CHECK(FormatTimePosition(TimeMode::Bbt, want, SR, map) == t);
    }

    std::printf("time_display_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
