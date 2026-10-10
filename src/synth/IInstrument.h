// IInstrument — the voice a MIDI track is played through.
//
// The analogue of IEffect (dsp/IEffect.h) for note sources: Prepare() off the
// realtime thread (may allocate), Render() on it (must not). An engine Bus owns
// one, built from an InstrumentDesc by MakeInstrument().
//
// The contract that matters: Render must be a pure function of its block start.
// No state carries between calls, so a seek, a loop-record restart, or a
// different block size all produce identical audio — which is why the engine
// can rebuild its whole graph at a loop seam without a click, and why an
// offline bounce matches live playback sample for sample. Implementations may
// keep scratch buffers (sized in Prepare) but must not let anything about one
// block's output depend on a previous call.
//
// Kit-free, host-testable.
#pragma once

#include "../model/types.h"
#include "../model/VoiceExpression.h"

#include <cmath>
#include <cstddef>
#include <vector>

namespace daw {

struct MidiNote;

// Per-channel gain at a block boundary. Render ramps between two of these so a
// controller the caller only samples once per block glides instead of stepping.
struct StereoGain {
    float l = 1.0f;
    float r = 1.0f;
};

// Mod-wheel vibrato — shared by every voice, so a track sounds the same
// whichever one plays it. CC1 drives a sine on pitch at this rate, reaching this
// many semitones at full wheel (a gentle, musical amount: enough to hear the
// wheel move, not a special effect).
constexpr double kModWheelVibratoHz    = 5.0;
constexpr double kModWheelVibratoSemis = 1.0;

// The phase advance (in frames) the vibrato has added `relFrames` into a note —
// the EXACT integral of the ratio A·sin(ω·t) over [0, rel]:
//
//     A·(1 − cos(ω·rel)) / ω,  ω = 2π·rate/sr,  A = depth·(2^(semis/12) − 1)
//
// A closed form, so it is pure in `relFrames`: a vibrating voice is still a pure
// function of its block, and a bounce still matches playback. It is 0 at every
// whole LFO period, so the wheel wobbles the note around its own pitch instead
// of walking it away from it.
inline double VibratoPhaseFrames(double relFrames, double sr, double depth) {
    if (depth <= 0.0 || sr <= 0.0) return 0.0;
    const double w = 2.0 * M_PI * kModWheelVibratoHz / sr;   // radians per frame
    if (w <= 0.0) return 0.0;
    const double a = depth * (std::pow(2.0, kModWheelVibratoSemis / 12.0) - 1.0);
    return a * (1.0 - std::cos(w * relFrames)) / w;
}

class IInstrument {
public:
    virtual ~IInstrument() = default;

    // Set the render sample rate. Off-RT; may allocate.
    virtual void Prepare(double sampleRate) = 0;

    // ADD every note sounding in [blockStart, blockStart + frames) into the
    // interleaved-stereo `out`, scaled per channel by a gain that ramps
    // linearly from `from` to `to` across the block. Additive: never clears
    // `out`. RT-safe: must not allocate, lock, or touch the filesystem.
    //
    // `expr` is the track's channel expression at the block start: pitch bend
    // (which moves the voice's phase/read position, never a re-derived
    // frequency — see model/MidiExpression.h) and the mod wheel. The default is
    // "no expression", which renders exactly what this interface rendered before
    // expression existed.
    //
    // Not const because implementations reuse scratch buffers, but no state
    // survives the call — see the class comment.
    virtual void Render(const std::vector<MidiNote>& notes,
                        float* out, size_t frames, Frame blockStart,
                        StereoGain from, StereoGain to,
                        const VoiceExpression& expr = {}) = 0;

    // Convenience: a constant per-channel gain across the block.
    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart,
                float gainL, float gainR, const VoiceExpression& expr = {}) {
        Render(notes, out, frames, blockStart,
               StereoGain{gainL, gainR}, StereoGain{gainL, gainR}, expr);
    }

    // Convenience: the same constant gain on both channels (centered).
    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart, float gain,
                const VoiceExpression& expr = {}) {
        Render(notes, out, frames, blockStart, gain, gain, expr);
    }

    virtual const char* Name() const = 0;
};

} // namespace daw
