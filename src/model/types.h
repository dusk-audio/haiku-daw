// Core value types for the DAW model layer.
//
// This layer is deliberately kit-free (no libbe / Media Kit): it uses only
// the C++ standard library so it compiles and unit-tests on any host, not
// just Haiku. The engine and UI layers convert these to/from Haiku types
// (BString, media_format, ...) at their own edges.
#pragma once

#include <cstdint>
#include <string>

namespace daw {

// The timeline unit is the audio *frame* (one sample per channel), stored
// as a signed 64-bit integer. Integer frames = sample-accurate positions
// with no floating-point drift. Seconds are a UI-edge convenience only.
using Frame = int64_t;

// Stable identifiers. Indices shift when things are inserted/removed, so
// commands and the UI refer to tracks/clips by id, never by index.
using TrackId = uint64_t;
using ClipId  = uint64_t;

constexpr TrackId kInvalidTrackId = 0;
constexpr ClipId  kInvalidClipId  = 0;

enum class TrackType { Audio, Midi, Bus };

struct TimeSignature {
    int numerator   = 4;
    int denominator = 4;
};

enum class TransportState { Stopped, Playing, Recording };

// Playhead + loop state. Positions are in frames on the project timeline.
struct Transport {
    TransportState state   = TransportState::Stopped;
    Frame          playhead = 0;
    bool           loopEnabled = false;
    Frame          loopStart   = 0;
    Frame          loopEnd     = 0;
    bool           punchEnabled = false;   // record only within [punchIn, punchOut)
    Frame          punchIn      = 0;
    Frame          punchOut     = 0;
};

// Seconds<->frames conversion lives here so the rule is in exactly one
// place. Everything internal stays in frames.
inline Frame SecondsToFrames(double seconds, double sampleRate) {
    return static_cast<Frame>(seconds * sampleRate + 0.5);
}
inline double FramesToSeconds(Frame frames, double sampleRate) {
    return static_cast<double>(frames) / sampleRate;
}

} // namespace daw
