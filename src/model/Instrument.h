// Instrument — per-(MIDI)-track synth voice settings: waveform + ADSR.
//
// The Synth is stateless (a block is a pure function of its start frame), so
// these are just parameters passed to Render — no per-voice state. Times are in
// seconds; sustain is a 0..1 level. Kit-free.
#pragma once

namespace daw {

enum class Waveform { Sine = 0, Saw = 1, Square = 2, Triangle = 3 };

struct Instrument {
    int   waveform = 0;       // Waveform enum
    float attack   = 0.005f;  // seconds (0 -> instant)
    float decay    = 0.050f;  // seconds
    float sustain  = 0.80f;   // 0..1 level after decay
    float release  = 0.060f;  // seconds (tail past note-off)
};

} // namespace daw
