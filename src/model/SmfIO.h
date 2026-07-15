// SmfIO — a self-contained Standard MIDI File (.mid) reader/writer.
//
// Parses/writes SMF format 0 and 1 directly (no Media/Midi Kit): header chunk,
// track chunks, variable-length delta times, running status, note on/off, and
// the tempo + track-name meta events. Everything else is skipped correctly so
// note timing stays exact. Kit-free (std C++ only) so it builds and unit-tests
// on any host and round-trips in a test.
//
// The neutral data model below is tempo/PPQ-relative (ticks), independent of
// the project's sample rate; the caller converts ticks<->frames via the
// project TempoMap (beat = tick / division).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// One note, in SMF ticks relative to the start of its track.
struct SmfNote {
    int      pitch      = 60;    // 0..127
    int      velocity   = 100;   // 1..127 (note-on velocity)
    uint32_t startTick  = 0;
    uint32_t lengthTick = 0;
};

struct SmfTrack {
    std::string          name;
    std::vector<SmfNote> notes;
};

struct SmfData {
    uint16_t             division   = 480;    // ticks per quarter note (PPQ)
    double               tempoBpm   = 120.0;  // first tempo found (or default)
    std::vector<SmfTrack> tracks;
};

// Parse a .mid file into `out`. Returns false on a missing file or malformed
// header/chunk. SMPTE-division files (negative division) are rejected.
bool ReadSmf(const std::string& path, SmfData& out);

// Write `in` as a format-1 SMF (one MTrk per track; the tempo meta goes at the
// head of the first track). Returns false if the file cannot be written.
bool WriteSmf(const std::string& path, const SmfData& in);

} // namespace daw
