// Instrument — per-(MIDI)-track voice settings.
//
// `Instrument` is the built-in synth's parameters: waveform + ADSR. The Synth
// is stateless (a block is a pure function of its start frame), so these are
// just parameters passed to Render — no per-voice state. Times are in seconds;
// sustain is a 0..1 level.
//
// `InstrumentDesc` is what a Track actually stores: which KIND of voice the
// track uses, plus the settings that kind needs. It is the serializable
// descriptor, the counterpart of EffectDesc in Effect.h — MakeInstrument turns
// one into the IInstrument the engine renders through.
//
// Kit-free.
#pragma once

#include <string>

namespace daw {

enum class Waveform { Sine = 0, Saw = 1, Square = 2, Triangle = 3 };

struct Instrument {
    int   waveform = 0;       // Waveform enum
    float attack   = 0.005f;  // seconds (0 -> instant)
    float decay    = 0.050f;  // seconds
    float sustain  = 0.80f;   // 0..1 level after decay
    float release  = 0.060f;  // seconds (tail past note-off)
};

// How a MIDI track is voiced. Append new kinds at the END and bump
// kMaxInstrumentTypeId — the value is written to project files, so the
// existing numbering must not shift (same rule as kMaxEffectTypeId).
enum class InstrumentType {
    Synth = 0,   // the built-in oscillator + ADSR
    Sfz   = 1,   // an .sfz multisample instrument
    Sf2   = 2,   // one preset of a .sf2 SoundFont
};
constexpr int kMaxInstrumentTypeId = 2;

struct InstrumentDesc {
    InstrumentType type = InstrumentType::Synth;

    // Used when type == Synth.
    Instrument  synth;

    // Used when type == Sfz / Sf2: the soundfont file, and for SF2 which
    // preset of it. The path is stored as given so a project stays portable
    // if the user keeps their library at a stable location.
    std::string path;
    int         sf2Preset = 0;

    bool UsesSoundfont() const { return type != InstrumentType::Synth; }
};

} // namespace daw
