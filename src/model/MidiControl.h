// MidiControl — evaluate a MIDI track's continuous controllers at a frame.
//
// MidiClipEvent stores CC / pitch-bend / program as sparse timed points; the
// engine and exporter need the *current* value at a given frame (a step
// function: the latest event at or before the frame). These helpers do that
// scan. Evaluate once per block, off the per-sample inner loop — a linear scan
// over a track's events, which are few.
//
// Kit-free, header-only, host-testable.
#pragma once

#include "Project.h"

#include <vector>

namespace daw {

// Value of controller `cc` at absolute frame `at`: the value of the latest CC
// event for that controller with startFrame <= at, else `def`. Events need not
// be sorted. Use absolute-frame events (Track::CollectEvents()).
inline int CcValueAt(const std::vector<MidiClipEvent>& events, int cc,
                     Frame at, int def) {
    int   best  = def;
    Frame bestF = -1;
    for (const MidiClipEvent& e : events) {
        if (e.type != MidiClipEvent::CC || e.data != cc) continue;
        if (e.startFrame <= at && e.startFrame >= bestF) {   // latest wins
            bestF = e.startFrame;
            best  = e.value;
        }
    }
    return best;
}

// 14-bit pitch bend at `at` (0..16383, 8192 = center), else 8192 (center).
inline int PitchBendAt(const std::vector<MidiClipEvent>& events, Frame at) {
    int   best  = 8192;
    Frame bestF = -1;
    for (const MidiClipEvent& e : events) {
        if (e.type != MidiClipEvent::PitchBend) continue;
        if (e.startFrame <= at && e.startFrame >= bestF) { bestF = e.startFrame; best = e.value; }
    }
    return best;
}

// Channel gain from CC7 (volume) x CC11 (expression) at `at`, each 0..127 mapped
// linearly to 0..1; absent controllers default to 127 (unity). This is the
// scalar the synth renders a MIDI track's notes through, so a MIDI file's volume
// / expression automation plays back. Returns a [0,1] linear gain.
inline float MidiChannelGain(const std::vector<MidiClipEvent>& events, Frame at) {
    const int vol  = CcValueAt(events, 7,  at, 127);
    const int expr = CcValueAt(events, 11, at, 127);
    return (float)vol / 127.0f * (float)expr / 127.0f;
}

} // namespace daw
