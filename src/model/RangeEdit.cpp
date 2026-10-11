#include "RangeEdit.h"

namespace daw {

bool SliceClipToRange(const Clip& c, Frame a, Frame b, Clip* out) {
    if (!out || b <= a) return false;
    const Frame cs = c.startFrame;
    const Frame ce = c.startFrame + c.lengthFrames;
    const Frame s0 = std::max(cs, a);
    const Frame s1 = std::min(ce, b);
    if (s1 <= s0) return false;

    Clip n = c;
    n.startFrame   = s0 - a;                  // range-relative
    n.lengthFrames = s1 - s0;
    n.sourceOffset = c.sourceOffset + (s0 - cs);
    n.fadeInFrames  = TrimFade(c.fadeInFrames,  s0 - cs, n.lengthFrames);
    n.fadeOutFrames = TrimFade(c.fadeOutFrames, ce - s1, n.lengthFrames);
    *out = n;
    return true;
}

bool SliceRegionToRange(const MidiClip& c, Frame a, Frame b, MidiClip* out) {
    if (!out || b <= a) return false;
    const Frame cs = c.startFrame;
    const Frame ce = c.startFrame + c.lengthFrames;
    const Frame s0 = std::max(cs, a);
    const Frame s1 = std::min(ce, b);
    if (s1 <= s0) return false;

    MidiClip n = c;
    n.startFrame   = s0 - a;                  // range-relative
    n.lengthFrames = s1 - s0;
    n.fadeInFrames  = TrimFade(c.fadeInFrames,  s0 - cs, n.lengthFrames);
    n.fadeOutFrames = TrimFade(c.fadeOutFrames, ce - s1, n.lengthFrames);

    // Content: keep what sounds inside the range and rebase it. The window of a
    // MIDI region decides what sounds, so a note straddling the range end is
    // shortened to it, and a note that starts before the range start does not
    // sound at all (it would be silent in the window anyway) — the copy is the
    // range, exactly.
    n.notes.clear();
    for (const MidiNote& note : c.notes) {
        const Frame absStart = cs + note.startFrame;
        if (absStart < s0 || absStart >= s1) continue;
        MidiNote nn = note;
        nn.startFrame = absStart - s0;
        const Frame room = s1 - absStart;
        if (nn.lengthFrames > room) nn.lengthFrames = room;
        if (nn.lengthFrames < 1) nn.lengthFrames = 1;
        n.notes.push_back(nn);
    }
    n.events.clear();
    for (const MidiClipEvent& e : c.events) {
        const Frame absStart = cs + e.startFrame;
        if (absStart < s0 || absStart >= s1) continue;
        MidiClipEvent ne = e;
        ne.startFrame = absStart - s0;
        n.events.push_back(ne);
    }
    *out = n;
    return true;
}

std::vector<RangeItem> ExtractRange(const Project& p, Frame a, Frame b) {
    std::vector<RangeItem> out;
    if (b <= a) return out;
    for (const Track& t : p.Tracks()) {
        for (const Clip& c : t.clips) {
            if (c.takeGroup > 0 && !c.takeActive) continue;   // hidden take
            RangeItem it;
            if (SliceClipToRange(c, a, b, &it.audio)) {
                it.midi  = false;
                it.track = t.id;
                out.push_back(it);
            }
        }
        for (const MidiClip& c : t.midiClips) {
            if (c.takeGroup > 0 && !c.takeActive) continue;
            RangeItem it;
            if (SliceRegionToRange(c, a, b, &it.region)) {
                it.midi  = true;
                it.track = t.id;
                out.push_back(it);
            }
        }
    }
    return out;
}

} // namespace daw
