// Synth — a tiny polyphonic sine synthesizer for MIDI playback.
//
// Renders a track's MIDI notes to interleaved-stereo float. Phase is derived
// from (globalFrame - noteStart), so a block is a pure function of its start
// frame — no per-note state to carry, which makes it trivially correct across
// block boundaries and safe to rebuild per playback (like the audio streams).
// A short attack/release envelope avoids clicks.
//
// Expression (model/MidiExpression.h) rides on that same derivation: pitch bend
// adds the *integral* of its ratio to the elapsed frames, and the mod wheel adds
// the closed form of the vibrato's integral — both pure functions of the frame,
// so a bent or wobbling note is still a pure function of its block.
//
// Kit-free, host-testable. v1 is sine-only; the interface stays the same when
// richer waveforms/wavetables arrive.
#pragma once

#include "IInstrument.h"        // StereoGain, VoiceExpression, vibrato
#include "../model/Project.h"

#include <cstddef>
#include <vector>

namespace daw {

class Synth {
public:
    explicit Synth(double sampleRate = 48000.0) : fSampleRate(sampleRate) {}
    void SetSampleRate(double sr) { if (sr > 0) fSampleRate = sr; }

    // Add every note sounding in [blockStart, blockStart+frames) into the
    // interleaved-stereo `out`, using the instrument's waveform + ADSR (release
    // rings past note-off), scaled per channel by a gain that RAMPS linearly from
    // `from` to `to` across the block. Separate L/R gains let the caller place the
    // voice with a MIDI channel pan (CC10) without a second pass over the buffer;
    // the ramp de-zippers the channel controllers, which the engine and exporter
    // evaluate only once per block — a stepped CC7/CC10/CC11 would otherwise jump
    // at every block boundary and click. Pass the previous block's `to` as the
    // next block's `from` and the gain is continuous across the whole render.
    // `from == to` is the common case and costs exactly what a constant gain did.
    // `expr` is the track's bend + mod wheel at the block start (default: none).
    // Never allocates.
    void Render(const std::vector<MidiNote>& notes, const Instrument& inst,
                float* out, size_t frames, Frame blockStart,
                StereoGain from, StereoGain to,
                const VoiceExpression& expr = {}) const;

    // Convenience: a constant per-channel gain across the block.
    void Render(const std::vector<MidiNote>& notes, const Instrument& inst,
                float* out, size_t frames, Frame blockStart,
                float gainL, float gainR, const VoiceExpression& expr = {}) const {
        Render(notes, inst, out, frames, blockStart,
               StereoGain{gainL, gainR}, StereoGain{gainL, gainR}, expr);
    }

    // Convenience: the same constant gain on both channels (centered).
    void Render(const std::vector<MidiNote>& notes, const Instrument& inst,
                float* out, size_t frames, Frame blockStart, float gain,
                const VoiceExpression& expr = {}) const {
        Render(notes, inst, out, frames, blockStart, gain, gain, expr);
    }

private:
    double fSampleRate;
};

} // namespace daw
