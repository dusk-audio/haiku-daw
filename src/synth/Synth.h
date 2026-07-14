// Synth — a tiny polyphonic sine synthesizer for MIDI playback.
//
// Renders a track's MIDI notes to interleaved-stereo float. Phase is derived
// from (globalFrame - noteStart), so a block is a pure function of its start
// frame — no per-note state to carry, which makes it trivially correct across
// block boundaries and safe to rebuild per playback (like the audio streams).
// A short attack/release envelope avoids clicks.
//
// Kit-free, host-testable. v1 is sine-only; the interface stays the same when
// richer waveforms/wavetables arrive.
#pragma once

#include "../model/Project.h"

#include <cstddef>
#include <vector>

namespace daw {

class Synth {
public:
    explicit Synth(double sampleRate = 48000.0) : fSampleRate(sampleRate) {}
    void SetSampleRate(double sr) { if (sr > 0) fSampleRate = sr; }

    // Add every note sounding in [blockStart, blockStart+frames) into the
    // interleaved-stereo `out`, scaled by `gain`, using the instrument's
    // waveform + ADSR (release rings past note-off). Never allocates.
    void Render(const std::vector<MidiNote>& notes, const Instrument& inst,
                float* out, size_t frames, Frame blockStart, float gain) const;

private:
    double fSampleRate;
};

} // namespace daw
