// Sf2Reader — a minimal SoundFont 2 (.sf2) metadata reader.
//
// Parses the pdta records (presets / instruments / samples and their generator
// zones) and records the byte range of the sdta sample chunk so a later pass
// can extract the PCM. Modulators, ROM samples and the INFO block are ignored —
// they don't affect the note mapping this feeds.
//
// Layout follows the SoundFont 2.04 spec: a RIFF('sfbk') containing
// LIST('INFO'), LIST('sdta') and LIST('pdta'). pdta holds fixed-size records
// (phdr/pbag/pgen/inst/ibag/igen/shdr) with terminal sentinel entries
// ("EOP"/"EOI"/"EOS") used only to bound the preceding record's index ranges.
//
// Ported from DuskStudio's src/engine/multisample/Sf2Reader.{h,cpp}, with JUCE
// swapped for std::string / std::ifstream. Pure data + STL, so it unit-tests
// against a real .sf2 fixture without an audio engine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// SF2 generator operators the converter reads. Values are from the spec's
// SFGenerator enum; the full set is larger, and unhandled opers pass through
// untouched.
enum Sf2Gen : uint16_t {
    kGenStartAddrsOffset   = 0,
    kGenInitialFilterFc    = 8,    // lowpass cutoff, absolute cents
    kGenInitialFilterQ     = 9,    // resonance, centibels
    kGenPan                = 17,
    kGenDelayVolEnv        = 33,
    kGenAttackVolEnv       = 34,
    kGenHoldVolEnv         = 35,
    kGenDecayVolEnv        = 36,
    kGenSustainVolEnv      = 37,
    kGenReleaseVolEnv      = 38,
    kGenInstrument         = 41,   // preset zone -> instrument index
    kGenKeyRange           = 43,
    kGenVelRange           = 44,
    kGenInitialAttenuation = 48,
    kGenCoarseTune         = 51,
    kGenFineTune           = 52,
    kGenSampleID           = 53,
    kGenSampleModes        = 54,
    kGenScaleTuning        = 56,
    kGenExclusiveClass     = 57,   // self-choking group (drum hi-hats)
    kGenOverridingRootKey  = 58,
};

struct Sf2Generator {
    uint16_t oper   = 0;
    uint16_t amount = 0;   // raw genAmount word; interpret per oper

    // Range generators (keyRange/velRange) pack lo in the low byte and hi in
    // the high byte.
    uint8_t lo() const { return (uint8_t)(amount & 0xff); }
    uint8_t hi() const { return (uint8_t)((amount >> 8) & 0xff); }
    int16_t asSigned() const { return (int16_t)amount; }
};

struct Sf2Zone {
    std::vector<Sf2Generator> gens;

    // First generator with this oper, or nullptr. SF2 zones list each oper at
    // most once.
    const Sf2Generator* Find(uint16_t oper) const;
};

struct Sf2Instrument {
    std::string          name;
    std::vector<Sf2Zone> zones;   // zone 0 may be global (no sampleID)
};

struct Sf2Preset {
    std::string          name;
    uint16_t             preset = 0;   // MIDI program
    uint16_t             bank   = 0;
    std::vector<Sf2Zone> zones;        // zone 0 may be global; others ref instruments
};

struct Sf2Sample {
    std::string name;
    uint32_t    start           = 0;   // sample frames into the smpl chunk
    uint32_t    end             = 0;
    uint32_t    startLoop       = 0;
    uint32_t    endLoop         = 0;
    uint32_t    sampleRate      = 0;
    uint8_t     originalPitch   = 60;
    int8_t      pitchCorrection = 0;   // cents
    uint16_t    sampleLink      = 0;   // paired sample index, for stereo
    uint16_t    sampleType      = 1;   // 1=mono,2=right,4=left,8=linked,0x8000=ROM
};

struct Sf2File {
    std::vector<Sf2Preset>     presets;
    std::vector<Sf2Instrument> instruments;
    std::vector<Sf2Sample>     samples;

    // Byte offset + length of the sdta 'smpl' chunk PCM (16-bit LE) in the
    // source file, for on-demand sample extraction. sm24 is the optional
    // 8-bit LSB extension for 24-bit samples (0 size = absent).
    int64_t smplOffset = 0;
    int64_t smplSize   = 0;
    int64_t sm24Offset = 0;
    int64_t sm24Size   = 0;

    bool        ok = false;
    std::string error;
};

// Parse an .sf2. On failure returns an Sf2File with ok=false and a populated
// error. Reads metadata only — never the sample PCM.
Sf2File ReadSf2(const std::string& path);

} // namespace daw
