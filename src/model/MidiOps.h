// MidiOps — pure MIDI note transforms: quantize (strength + swing), humanize,
// legato, transpose, velocity scaling.
//
// Kit-free and UI-free: the piano roll runs these on its snapshot and posts the
// RESULT (see kMsgApplyMidiOp), which keeps the math reachable from a host test
// and keeps the model command free of selection indices that a changed note
// list would re-aim. Positions are clip-relative frames like MidiNote; every
// time-domain transform converts to absolute BEATS through the project's
// TempoMap first, so a grid position stays musically right even when a tempo
// change or a ramp falls inside the region.
//
// The region is a non-destructive window (see MidiClip): content outside
// [0, lengthFrames) is kept but silent. A transform therefore never touches a
// note that is already outside the window (snapping one back in would make it
// suddenly sound), and never lets a note it moved leave the window -- either
// outcome would silently change what the region plays, and a note ending past
// the window would also make the region grow (the notes command only ever
// grows it).
#pragma once

#include "Project.h"
#include "TempoMap.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace daw {

// A selection mask, index-aligned with the note list (the piano roll's fSel).
// An EMPTY mask means "every note" -- what a transform does when nothing is
// selected.
using NoteSel = std::vector<char>;

inline bool NoteSelected(const NoteSel& sel, std::size_t i) {
    return sel.empty() || (i < sel.size() && sel[i] != 0);
}

// Compare two note lists field by field. Used to skip committing a transform
// that moved nothing: the command stack has no no-op detection, so a no-op
// would otherwise push an undo entry that undoes to the same state.
bool NotesEqual(const std::vector<MidiNote>& a, const std::vector<MidiNote>& b);

// The musical step notes snap to, in beats (a quarter note is one beat; the
// project's BPM is quarter-note based). The triplet grids are "three in the
// space of two" -- the subdivisions a swing feel is built on.
enum class QuantGrid {
    Quarter, Eighth, Sixteenth, ThirtySecond,
    QuarterTriplet, EighthTriplet, SixteenthTriplet,
};
double      GridStepBeats(QuantGrid g);
const char* GridName(QuantGrid g);   // "1/4", "1/8T", ... (the settings dialog)

struct QuantizeOpts {
    QuantGrid grid            = QuantGrid::Sixteenth;
    float     strength        = 1.0f;   // 0 = unchanged, 1 = fully on the grid
    float     swingPct        = 0.0f;   // 0 = straight, 100 = full triplet feel
    bool      quantizeLengths = false;  // snap note ends to the grid as well
};

// Which transform a posted result came from. Carried by the piano roll so the
// model command can name the undo step ("Undo Quantize").
enum class MidiOp { Quantize, Humanize, Legato, Transpose, Velocity };
const char* MidiOpName(MidiOp op);

// Snap the selected notes to the grid, pulling each one `strength` of the way
// from where it is to where the grid says it should be. `swingPct` delays every
// second grid slot by up to a third of a step (a full triplet feel) -- starts
// only, never ends: an end that swung would shorten the note that swung into
// it. With `quantizeLengths`, ends snap too (unswung).
std::vector<MidiNote> Quantize(const std::vector<MidiNote>& notes,
                               const NoteSel& sel, const TempoMap& tempo,
                               Frame clipStart, Frame clipLen,
                               const QuantizeOpts& opts);

// Deterministic jitter: the same seed and the same notes always produce the
// same result, so a take can be reproduced. Timing moves by at most
// `timingJitterFrames` either way, velocity by `velocityJitter` (clamped to
// 1..127); starts stay >= 0. Deliberately NOT idempotent -- humanizing twice is
// a fresh take, which is the point.
std::vector<MidiNote> Humanize(const std::vector<MidiNote>& notes,
                               const NoteSel& sel, Frame clipLen,
                               Frame timingJitterFrames, int velocityJitter,
                               uint64_t seed);

// Extend each selected note to the start of the next note that begins after it,
// whatever its pitch (monophonic-legato: a line is continued by whatever comes
// next). A note already overlapping the next one is left alone -- legato only
// ever extends. The last note in the region keeps its length.
std::vector<MidiNote> Legato(const std::vector<MidiNote>& notes,
                             const NoteSel& sel, Frame clipLen);

// Pitch, clamped to 0..127.
std::vector<MidiNote> TransposeSemitones(const std::vector<MidiNote>& notes,
                                         const NoteSel& sel, int semitones);

// v' = clamp(round(v * mul + add), 1, 127).
std::vector<MidiNote> ScaleVelocity(const std::vector<MidiNote>& notes,
                                    const NoteSel& sel, float mul, float add);

} // namespace daw
