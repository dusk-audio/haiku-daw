// A single live MIDI event — the raw unit that flows in from a keyboard (or out
// to a hardware synth), before it is paired into the model's note pairs.
//
// Deliberately kit-free: no bigtime_t / uchar / BMidi types. The Midi Kit 2
// adapter (src/midi/MidiPort) converts the kit's hook arguments into these at
// its edge, so the recording/playback logic below it stays host-testable.
#pragma once

#include <cstdint>

namespace daw {

struct MidiEvent {
    enum Type : uint8_t {
        kNoteOff = 0,
        kNoteOn,
        kControlChange,
        kPitchBend,
        kProgramChange,
        kOther,
    };

    Type     type    = kOther;
    uint8_t  channel = 0;   // 0..15
    uint8_t  data1   = 0;   // note number / CC number / program number
    uint8_t  data2   = 0;   // velocity / CC value (0 for 1-byte messages)
    int16_t  bend    = 0;   // pitch bend, -8192..8191 (kPitchBend only)
    int64_t  timeUs  = 0;   // arrival time in the system_time() microsecond
                            // domain; 0 = "now" / unstamped
    // Which endpoint this arrived from (the Midi Kit producer id), so several
    // keyboards can be told apart and routed to different tracks. Stamped by
    // MidiInputPort at the kit edge. 0 = unknown/untagged, which every route
    // accepts — host tests and loopback sources never set it. Kept LAST so the
    // aggregate initializers above stay valid.
    int32_t  source  = 0;

    static MidiEvent NoteOn(uint8_t ch, uint8_t note, uint8_t vel, int64_t t = 0) {
        // Running-status convention: a note-on with velocity 0 is a note-off.
        return { vel ? kNoteOn : kNoteOff, ch, note, vel, 0, t };
    }
    static MidiEvent NoteOff(uint8_t ch, uint8_t note, uint8_t vel, int64_t t = 0) {
        return { kNoteOff, ch, note, vel, 0, t };
    }
    static MidiEvent ControlChange(uint8_t ch, uint8_t cc, uint8_t val, int64_t t = 0) {
        return { kControlChange, ch, cc, val, 0, t };
    }

    // A note-off in effect: an explicit note-off, or a note-on with velocity 0.
    bool IsNoteOff() const { return type == kNoteOff || (type == kNoteOn && data2 == 0); }
    bool IsNoteOn()  const { return type == kNoteOn && data2 > 0; }
};

} // namespace daw
