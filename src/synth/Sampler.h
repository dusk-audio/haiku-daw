// Sampler — plays a LoadedInstrument (an SFZ or a converted SF2 preset).
//
// Stateless across blocks, exactly like Synth: a voice's read position is
// (globalFrame - noteStart) * pitchRatio, the same derivation Synth uses for
// oscillator phase, and loop wrap is modulo arithmetic on it. So a block is a
// pure function of its start frame and nothing has to be carried, reset on
// seek, or kept in sync between the live engine and the offline exporter.
//
// The per-block voice list is rebuilt from the note vector every call into a
// scratch buffer sized in Prepare(), so Render never allocates.
//
// Not implemented in v1: round-robin (seq_position) and random region
// selection, which genuinely need state; filters and LFOs. Regions carrying
// those opcodes still play, just without that modulation.
//
// Kit-free, host-testable.
#pragma once

#include "IInstrument.h"
#include "SampleBank.h"

#include <cstddef>
#include <vector>

namespace daw {

struct MidiNote;

class Sampler : public IInstrument {
public:
    // A null or empty instrument renders silence rather than failing, so a
    // project whose sample folder moved still opens and plays.
    explicit Sampler(LoadedInstrumentPtr inst, double sampleRate = 48000.0);

    void Prepare(double sampleRate) override;

    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart,
                StereoGain from, StereoGain to,
                const VoiceExpression& expr = {}) override;

    using IInstrument::Render;   // keep the constant-gain convenience overloads

    const char* Name() const override { return "Sampler"; }

    // On top of the interface's contract: pitch bend and the mod wheel move the
    // voice's READ POSITION, not the region's tuning, so a bend slides the
    // sample exactly as an oscillator's phase slides (see model/MidiExpression.h
    // and IInstrument.h). The position is derived from the note's own age, so a
    // bent voice is still a pure function of its block.

    // Simultaneously sounding region-voices. A single note can open several
    // (SFZ layers every matching region), so this is well above a polyphony
    // count. fVoices is reserved to exactly this and NEVER grows past it, so
    // Render allocates nothing.
    //
    // Over the cap the most recently started voices win, decided by bounded
    // insertion rather than by sorting (std::stable_sort allocates a temporary
    // buffer, which the RT thread must not do). The cap is a CPU safety valve
    // for pathological content, not a musical polyphony limit: real kits open
    // well under a dozen voices per note. Note that WHICH voices survive can
    // differ between block sizes once the cap engages, so a bounce could
    // diverge from playback there — another reason to keep it far above what
    // real material reaches.
    static constexpr int kMaxVoices = 256;

private:
    // One region firing for one note, resolved for this block.
    struct Voice {
        const Region* region  = nullptr;
        Frame         start   = 0;    // output frame the sample starts at
        Frame         off     = 0;    // output frame of note-off
        Frame         end     = 0;    // output frame the voice goes silent
        Frame         chokeAt = -1;   // output frame a choke group cuts it (-1 = none)
        float         amp     = 1.0f; // velocity x volume, pre-pan
        double        ratio   = 1.0;  // source frames per output frame
        int           order   = 0;    // note index, for a deterministic voice cap
        double        bend    = 0.0;  // the note's bend phase at its start (MidiExpression)
    };

    // Rebuild fVoices for the block, applying round-robin selection, choke
    // groups and the voice cap.
    void GatherVoices(const std::vector<MidiNote>& notes, Frame blockStart,
                      Frame blockEnd);

    // Add one voice, honouring the cap without ever growing fVoices: when full,
    // replace the oldest-started voice if this one is newer, else drop it.
    void AddVoice(const Voice& v);

    // How many times key `pitch` has already been struck before `before` —
    // the SFZ sequence counter. Only called for instruments using seq_length.
    int SeqIndex(const std::vector<MidiNote>& notes, int pitch, Frame before,
                 int orderBefore) const;

    // Earliest frame after `after` at which a note triggers a region in choke
    // group `group`, or -1. Only called for the rare region that sets off_by.
    Frame ChokeTime(const std::vector<MidiNote>& notes, int group, Frame after) const;

    LoadedInstrumentPtr fInst;
    double              fSampleRate;
    std::vector<Voice>  fVoices;   // scratch, reserved in Prepare
};

} // namespace daw
