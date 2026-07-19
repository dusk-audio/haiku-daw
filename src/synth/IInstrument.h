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
    // Not const because implementations reuse scratch buffers, but no state
    // survives the call — see the class comment.
    virtual void Render(const std::vector<MidiNote>& notes,
                        float* out, size_t frames, Frame blockStart,
                        StereoGain from, StereoGain to) = 0;

    // Convenience: a constant per-channel gain across the block.
    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart,
                float gainL, float gainR) {
        Render(notes, out, frames, blockStart,
               StereoGain{gainL, gainR}, StereoGain{gainL, gainR});
    }

    // Convenience: the same constant gain on both channels (centered).
    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart, float gain) {
        Render(notes, out, frames, blockStart, gain, gain);
    }

    virtual const char* Name() const = 0;
};

} // namespace daw
