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

// CC1 (mod wheel) at `at`, 0..127; absent = 0 (wheel all the way down). The
// voices turn this into vibrato depth — see model/MidiExpression.h.
inline int ModWheelAt(const std::vector<MidiClipEvent>& events, Frame at) {
    return CcValueAt(events, 1, at, 0);
}

// The value a controller is at when the region has no event for it — what the
// piano roll's lane draws before its first point, and what the engine renders
// there. It has to agree with the evaluators above: 127 for the two gain
// controllers and 64 for pan, and 0 for everything else (a switch is "off", and
// an unmodelled controller does not move a voice at all).
inline int CcDefault(int cc) {
    if (cc == 10)       return 64;    // pan   -> centre
    if (cc == 7 || cc == 11) return 127;   // volume / expression -> unity
    return 0;
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

// CC10 (pan) at `at` as a position in [-1, 1]; absent/64 = centered (0).
inline float MidiChannelPan(const std::vector<MidiClipEvent>& events, Frame at) {
    const int pan = CcValueAt(events, 10, at, 64);
    float p = ((float)pan - 64.0f) / 63.0f;
    if (p < -1.0f) p = -1.0f;
    if (p >  1.0f) p =  1.0f;
    return p;
}

// Per-channel gains a MIDI track's notes render through: the CC7 x CC11 scalar
// (MidiChannelGain) placed by the CC10 pan. The pan is a BALANCE law — unity at
// center, attenuating only the opposite channel — deliberately not equal-power:
// the synth voice is already written to both channels, and an equal-power law
// would pull a centered channel down 3 dB, changing the level of every existing
// project that has no CC10. With CC10 absent this is exactly gL == gR ==
// MidiChannelGain, so playback and bounces are unchanged.
inline void MidiChannelGains(const std::vector<MidiClipEvent>& events, Frame at,
                             float* gL, float* gR) {
    const float g = MidiChannelGain(events, at);
    const float p = MidiChannelPan(events, at);
    if (gL) *gL = g * ((p > 0.0f) ? (1.0f - p) : 1.0f);
    if (gR) *gR = g * ((p < 0.0f) ? (1.0f + p) : 1.0f);
}

} // namespace daw
