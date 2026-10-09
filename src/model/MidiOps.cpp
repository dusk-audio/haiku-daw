#include "MidiOps.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

// Splitmix64's finaliser, the same stateless hash the sampler uses for its
// deterministic per-note draws (synth/Sampler.cpp NoteRandom): humanize must be
// reproducible from (seed, note) alone, so nothing here keeps a running state.
inline uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// A symmetric draw in [-mag, +mag] from the note's own hash. 0 draws 0.
// The subtraction is done 64-bit: a large mag (up to INT_MAX for the velocity
// jitter) makes h % span exceed INT_MAX, and subtracting in int would be signed
// overflow -- UB the sanitizer flags, even though the wrapped value happens to
// be the right one.
inline int Jitter(uint64_t h, int mag) {
    if (mag <= 0) return 0;
    const uint32_t span = (uint32_t)(2 * (uint32_t)mag + 1u);
    return (int)((int64_t)(h % span) - (int64_t)mag);
}

// Does the region window let this note sound? Content outside [0, clipLen) is
// kept but silent by design, so every time-domain transform skips it: snapping
// a silent note back in would make it suddenly audible, and moving it further
// out is not what the user asked for either.
inline bool InWindow(const MidiNote& n, Frame clipLen) {
    return n.startFrame >= 0 && n.startFrame < clipLen;
}

// The two halves of the window rule, kept separate because the order matters:
// a transform that derives a LENGTH must measure it from where the start
// actually lands, so it clamps the start first (see Quantize).
inline void ClampStartToWindow(MidiNote& n, Frame clipLen) {
    if (clipLen < 1) clipLen = 1;
    if (n.startFrame < 0) n.startFrame = 0;
    if (n.startFrame >= clipLen) n.startFrame = clipLen - 1;
}

// The end rule. Callers pass only notes that were INSIDE the window (InWindow
// above), so this is about what the transform did to them: the notes command
// grows a region to fit anything past its end, and a transform must not resize
// the region as a side effect -- nor silence the note it just moved by pushing
// its start out the back.
inline void ClampEndToWindow(MidiNote& n, const MidiNote& orig, Frame clipLen) {
    if (clipLen < 1) clipLen = 1;
    const Frame origEnd = orig.startFrame
                        + std::max<Frame>(1, orig.lengthFrames);
    if (n.lengthFrames < 1) n.lengthFrames = 1;
    if (origEnd <= clipLen && n.startFrame + n.lengthFrames > clipLen)
        n.lengthFrames = clipLen - n.startFrame;
    if (n.lengthFrames < 1) n.lengthFrames = 1;
}

inline void ClampToWindow(MidiNote& n, const MidiNote& orig, Frame clipLen) {
    ClampStartToWindow(n, clipLen);
    ClampEndToWindow(n, orig, clipLen);
}

} // namespace

bool NotesEqual(const std::vector<MidiNote>& a, const std::vector<MidiNote>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (a[i].pitch != b[i].pitch || a[i].velocity != b[i].velocity
            || a[i].startFrame != b[i].startFrame
            || a[i].lengthFrames != b[i].lengthFrames)
            return false;
    return true;
}

double GridStepBeats(QuantGrid g) {
    switch (g) {
        case QuantGrid::Quarter:          return 1.0;
        case QuantGrid::Eighth:           return 0.5;
        case QuantGrid::Sixteenth:        return 0.25;
        case QuantGrid::ThirtySecond:     return 0.125;
        case QuantGrid::QuarterTriplet:   return 2.0 / 3.0;
        case QuantGrid::EighthTriplet:    return 1.0 / 3.0;
        case QuantGrid::SixteenthTriplet: return 1.0 / 6.0;
    }
    return 0.25;
}

const char* GridName(QuantGrid g) {
    switch (g) {
        case QuantGrid::Quarter:          return "1/4";
        case QuantGrid::Eighth:           return "1/8";
        case QuantGrid::Sixteenth:        return "1/16";
        case QuantGrid::ThirtySecond:     return "1/32";
        case QuantGrid::QuarterTriplet:   return "1/4T";
        case QuantGrid::EighthTriplet:    return "1/8T";
        case QuantGrid::SixteenthTriplet: return "1/16T";
    }
    return "1/16";
}

const char* MidiOpName(MidiOp op) {
    switch (op) {
        case MidiOp::Quantize:  return "Quantize";
        case MidiOp::Humanize:  return "Humanize";
        case MidiOp::Legato:    return "Legato";
        case MidiOp::Transpose: return "Transpose";
        case MidiOp::Velocity:  return "Adjust Velocity";
    }
    return "Edit Notes";   // unknown op from a malformed message
}

std::vector<MidiNote> Quantize(const std::vector<MidiNote>& notes,
                               const NoteSel& sel, const TempoMap& tempo,
                               Frame clipStart, Frame clipLen,
                               const QuantizeOpts& opts) {
    std::vector<MidiNote> out = notes;
    const double step = GridStepBeats(opts.grid);
    if (!(step > 0.0)) return out;   // also catches NaN
    // "Not a number in range" means no movement, which is the safe reading of
    // garbage: the plain comparisons would let a NaN through to the arithmetic
    // below, where it lands every note on frame 0.
    double strength = (double)opts.strength;
    if (!(strength >= 0.0)) strength = 0.0;
    if (strength > 1.0) strength = 1.0;
    double swing = (double)opts.swingPct;
    if (!(swing >= 0.0)) swing = 0.0;
    if (swing > 100.0) swing = 100.0;
    swing /= 100.0;

    for (std::size_t i = 0; i < out.size(); i++) {
        if (!NoteSelected(sel, i)) continue;
        const MidiNote orig = notes[i];
        if (!InWindow(orig, clipLen)) continue;

        // Absolute beats, so the grid is the SONG's grid even when the region
        // starts off-beat or a tempo change sits inside it.
        const double beat = tempo.BeatAt(clipStart + orig.startFrame);
        const double slot = std::llround(beat / step);
        // Swing delays every second slot, by up to a third of a step: at 100%
        // an off-beat lands two-thirds of the way through its pair, which is
        // the classic triplet feel.
        double target = slot * step;
        if (((long long)slot & 1LL) != 0) target += swing * step / 3.0;
        const double newBeat = beat + strength * (target - beat);

        MidiNote n = orig;
        n.startFrame = tempo.FrameAt(newBeat) - clipStart;
        // Clamp the start BEFORE measuring a new length: at the region edge the
        // clamp moves the note, and a length measured from the pre-clamp start
        // would put the end that many frames past the grid line the code itself
        // computed (and make a second quantize move it again).
        ClampStartToWindow(n, clipLen);

        if (opts.quantizeLengths) {
            const Frame endRel = orig.startFrame
                               + std::max<Frame>(1, orig.lengthFrames);
            const double endBeat = tempo.BeatAt(clipStart + endRel);
            const double endSlot = std::llround(endBeat / step);
            const double newEndBeat =
                endBeat + strength * (endSlot * step - endBeat);
            n.lengthFrames = tempo.FrameAt(newEndBeat) - clipStart - n.startFrame;
        }
        ClampToWindow(n, orig, clipLen);
        out[i] = n;
    }
    return out;
}

std::vector<MidiNote> Humanize(const std::vector<MidiNote>& notes,
                               const NoteSel& sel, Frame clipLen,
                               Frame timingJitterFrames, int velocityJitter,
                               uint64_t seed) {
    std::vector<MidiNote> out = notes;
    int mag = 0;
    if (timingJitterFrames > 0) {
        const Frame capped = std::min<Frame>(timingJitterFrames, 1 << 30);
        mag = (int)capped;
    }
    for (std::size_t i = 0; i < out.size(); i++) {
        if (!NoteSelected(sel, i)) continue;
        const MidiNote orig = notes[i];
        if (!InWindow(orig, clipLen)) continue;
        // Hash the note's identity AND its place in the list: two notes at the
        // same frame and pitch must move independently, and the index is what
        // separates them. The price is that inserting or removing a note
        // re-rolls the draws of the ones after it -- humanize is a fresh take,
        // not a property of the notes.
        const uint64_t h = Mix(seed
            ^ (uint64_t)i * 0x9E3779B97F4A7C15ull
            ^ ((uint64_t)(uint32_t)orig.startFrame << 1)
            ^ (uint64_t)(uint32_t)orig.pitch);
        MidiNote n = orig;
        n.startFrame += (Frame)Jitter(h, mag);
        // Jitter takes a magnitude wide enough for INT_MAX (see its comment),
        // and velocity + that would overflow int here: a velocity can only
        // move within 1..127, so 127 is as much magnitude as means anything.
        const int vmag = velocityJitter < 127 ? velocityJitter : 127;
        n.velocity = std::clamp(orig.velocity + Jitter(Mix(h), vmag), 1, 127);
        ClampToWindow(n, orig, clipLen);
        out[i] = n;
    }
    return out;
}

std::vector<MidiNote> Legato(const std::vector<MidiNote>& notes,
                             const NoteSel& sel, Frame clipLen) {
    std::vector<MidiNote> out = notes;
    for (std::size_t i = 0; i < out.size(); i++) {
        if (!NoteSelected(sel, i)) continue;
        const MidiNote orig = notes[i];
        if (!InWindow(orig, clipLen)) continue;
        // The earliest note starting after this one, any pitch. Strictly after,
        // so two notes on the same frame can never produce a zero-length note.
        Frame next = 0;
        bool found = false;
        for (std::size_t j = 0; j < notes.size(); j++) {
            if (j == i) continue;
            const Frame s = notes[j].startFrame;
            if (s <= orig.startFrame) continue;
            if (!found || s < next) { next = s; found = true; }
        }
        if (!found) continue;   // the last note keeps its length
        MidiNote n = orig;
        const Frame toNext = next - orig.startFrame;
        if (toNext > n.lengthFrames) n.lengthFrames = toNext;   // extend only
        ClampToWindow(n, orig, clipLen);
        out[i] = n;
    }
    return out;
}

std::vector<MidiNote> TransposeSemitones(const std::vector<MidiNote>& notes,
                                         const NoteSel& sel, int semitones) {
    std::vector<MidiNote> out = notes;
    for (std::size_t i = 0; i < out.size(); i++) {
        if (!NoteSelected(sel, i)) continue;
        out[i].pitch = std::clamp(out[i].pitch + semitones, 0, 127);
    }
    return out;
}

std::vector<MidiNote> ScaleVelocity(const std::vector<MidiNote>& notes,
                                    const NoteSel& sel, float mul, float add) {
    std::vector<MidiNote> out = notes;
    for (std::size_t i = 0; i < out.size(); i++) {
        if (!NoteSelected(sel, i)) continue;
        const float v = (float)out[i].velocity * mul + add;
        out[i].velocity = std::clamp((int)std::lround(v), 1, 127);
    }
    return out;
}

} // namespace daw
