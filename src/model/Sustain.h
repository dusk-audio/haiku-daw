// Sustain — the CC64 pedal's release policy, in one place.
//
// The policy, stated once and implemented twice (a live form the engine drives
// event by event, and an offline form that applies it to a region's notes):
//
//   * CC64 >= 64 latches. A note-off that arrives while the pedal is down is
//     DEFERRED: the note keeps sounding until the pedal lifts, or until the same
//     key is struck again (a re-strike releases the deferred note and retriggers
//     it), whichever comes first.
//   * A note-off while the pedal is up releases immediately.
//   * Notes still sounding when the pedal lifts keep sounding until their own
//     note-off: the pedal only releases what it was holding.
//
// A pedal-extended note is simply a longer note, so both voices (Synth and
// Sampler) honour it with no sustain code of their own, and the live form only
// has to decide *when* a voice's note-off lands.
//
// Kit-free, host-testable.
#pragma once

#include "Project.h"

#include <algorithm>
#include <numeric>
#include <vector>

namespace daw {

// CC64 counts as down from here (half-pedalling is not modelled).
constexpr int kSustainDownFrom = 64;

// --- the live form ---------------------------------------------------------

// The state machine the engine drives from the incoming event stream. It owns
// the pedal state and the keys the pedal is holding, so the policy above can be
// asserted on the host without an engine (see tests/midi_expression_tests.cpp).
//
// Allocation-free: the held-key list is a fixed array, sized like the engine's
// live-voice pool. If it ever overflows, the latch stops remembering new holds —
// the engine still releases its own sustained voices when the pedal lifts, so
// nothing is left hanging; only the re-strike shortcut is lost for that key.
class SustainLatch {
public:
    static constexpr int kMaxHeld = 64;

    void Reset() { fDown = false; fHeldCount = 0; }

    bool Down() const { return fDown; }

    // Feed a CC64 value. Returns true when the pedal LIFTED: the caller must
    // release every note it is holding (the latch forgets them here).
    bool SetValue(int ccValue) {
        const bool down = ccValue >= kSustainDownFrom;
        const bool lifted = fDown && !down;
        fDown = down;
        if (lifted) fHeldCount = 0;
        return lifted;
    }

    // A note-off arrived for `key` (the caller's identity — pitch alone, or
    // channel * 128 + pitch to keep two keyboards' keys apart). True means the
    // pedal is down: the caller must NOT release the note now. It will be
    // released when SetValue() reports the lift, or by a re-strike.
    bool DeferNoteOff(int key) {
        if (!fDown) return false;
        if (!Holding(key) && fHeldCount < kMaxHeld)
            fHeld[fHeldCount++] = key;
        return true;
    }

    // A note-on arrived for `key`. True means the pedal was holding that key:
    // this is a re-strike, so the caller releases the deferred note and starts
    // the new one.
    bool Retrigger(int key) {
        for (int i = 0; i < fHeldCount; i++) {
            if (fHeld[i] != key) continue;
            fHeld[i] = fHeld[--fHeldCount];
            return true;
        }
        return false;
    }

    bool Holding(int key) const {
        for (int i = 0; i < fHeldCount; i++)
            if (fHeld[i] == key) return true;
        return false;
    }
    int HeldCount() const { return fHeldCount; }

private:
    bool fDown      = false;
    int  fHeld[kMaxHeld] = {};
    int  fHeldCount = 0;
};

// --- the offline form ------------------------------------------------------

// CC64 in force at `at` (the latest CC64 event at or before it), default up.
inline bool SustainDownAt(const std::vector<MidiClipEvent>& events, Frame at) {
    int   best  = 0;
    Frame bestF = -1;
    for (const MidiClipEvent& e : events) {
        if (e.type != MidiClipEvent::CC || e.data != 64) continue;
        if (e.startFrame <= at && e.startFrame >= bestF) {
            bestF = e.startFrame;
            best  = e.value;
        }
    }
    return best >= kSustainDownFrom;
}

// The first frame after `after` at which CC64 goes up, or -1 if it never does
// inside this region.
inline Frame NextPedalUp(const std::vector<MidiClipEvent>& events, Frame after) {
    Frame best = -1;
    for (const MidiClipEvent& e : events) {
        if (e.type != MidiClipEvent::CC || e.data != 64) continue;
        if (e.value >= kSustainDownFrom) continue;
        if (e.startFrame <= after) continue;
        if (best < 0 || e.startFrame < best) best = e.startFrame;
    }
    return best;
}

// Apply the pedal to a region's notes, in place and in the region's own frame
// base (clip-relative for a MIDI region). Extends a note's sound to the pedal
// lift when its key-up lands under the pedal, capped by:
//   * `maxEnd`  — the region window, so a note can still never sound outside its
//                 region (the rule MidiOps.h protects);
//   * the next note of the same pitch — the re-strike case, matching the live
//                 latch's Retrigger().
// Never shortens a note and never moves a start. A region with no CC64 is
// untouched, so existing projects render bit-identically.
inline void ApplySustain(std::vector<MidiNote>& notes,
                         const std::vector<MidiClipEvent>& events,
                         Frame maxEnd) {
    bool any = false;
    for (const MidiClipEvent& e : events)
        if (e.type == MidiClipEvent::CC && e.data == 64) { any = true; break; }
    if (!any || notes.empty()) return;

    // Index by (pitch, start) once so "the next re-strike of this key" is a
    // neighbour in the sorted order rather than a scan per note.
    std::vector<int> order(notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (notes[(size_t)a].pitch != notes[(size_t)b].pitch)
            return notes[(size_t)a].pitch < notes[(size_t)b].pitch;
        if (notes[(size_t)a].startFrame != notes[(size_t)b].startFrame)
            return notes[(size_t)a].startFrame < notes[(size_t)b].startFrame;
        return a < b;
    });
    std::vector<Frame> retrigger(notes.size(), -1);
    for (size_t k = 0; k + 1 < order.size(); k++) {
        const size_t a = (size_t)order[k], b = (size_t)order[k + 1];
        if (notes[a].pitch == notes[b].pitch)
            retrigger[a] = notes[b].startFrame;
    }

    for (size_t i = 0; i < notes.size(); i++) {
        MidiNote& n = notes[i];
        const Frame off = n.startFrame + n.lengthFrames;
        if (off <= 0 || !SustainDownAt(events, off)) continue;

        Frame end = NextPedalUp(events, off);          // -1 = never lifts here
        if (end < 0 || end > maxEnd) end = maxEnd;
        if (retrigger[i] >= 0 && retrigger[i] < end) end = retrigger[i];
        if (end > off) n.lengthFrames = end - n.startFrame;
    }
}

} // namespace daw
