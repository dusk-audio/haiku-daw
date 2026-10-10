// VoiceExpression — what a track's continuous controllers hand a voice for one
// block: pitch bend (as a ratio plus its integrated phase) and the mod wheel.
//
// Its own tiny header so `src/synth/IInstrument.h` can take one without pulling
// the whole project model into every voice translation unit. The evaluation
// helpers that fill it from a track's events live in `MidiExpression.h`.
//
// Kit-free.
#pragma once

namespace daw {

struct VoiceExpression {
    // The frequency multiplier a voice advances at across this block: the MEAN
    // ratio over it, (Δ(end) - Δ(start)) / frames — not the ratio in force at
    // the block start. What matters is that a voice extrapolates its phase with
    // this number, so the phase it reaches at the block's end is where the next
    // block's `bendPhase` says it is: exact at every seam, including a block a
    // bend event lands inside. 1.0 = no bend.
    float  bendRatio = 1.0f;
    // The bend's integrated phase at the block start, in the frame units the
    // render is in: ∫₀^blockStart (bendRatio(t) − 1) dt. Subtracting the same
    // quantity at a note's start is what keeps a voice's phase continuous when
    // the ratio steps — see MidiExpression.h.
    double bendPhase = 0.0;
    // CC1, 0..1. The depth of the voice's vibrato; 0 = none.
    float  modWheel  = 0.0f;
};

} // namespace daw
