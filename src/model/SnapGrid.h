// SnapGrid — which musical grid the arrangement's edits snap to.
//
// Kit-free and header-only, so the menu's labels, the snap indicator and the
// snapping mathematics come from ONE definition and cannot drift apart -- and
// so the whole behaviour is host-testable rather than only reachable by opening
// a popup on the target machine.
//
// Snapping happens in BEAT space through the project's TempoMap (the same
// conversion the ruler draws with), so a tempo ramp or a meter change moves the
// grid with the music. A "division" here is in quarter-note beats: `1/8` is two
// steps per beat, `1/2` is half a step per beat, and a triplet takes 3 steps in
// the space of the 2 the plain value would -- exactly what `1/8T` means.
#pragma once

#include "TempoMap.h"
#include "types.h"

#include <cmath>

namespace daw {

enum class SnapKind {
    Off = 0,
    Bar,          // one bar of the governing meter
    Half,         // 1/2 note
    Quarter,      // 1/4 note (one beat)
    Eighth,       // 1/8 note
    Sixteenth,    // 1/16 note  (the editor's historical default)
    ThirtySecond, // 1/32 note
};

struct SnapGrid {
    SnapKind kind    = SnapKind::Sixteenth;
    bool     triplet = false;

    bool On() const { return kind != SnapKind::Off; }
};

// Steps per quarter-note beat for a note value (1/2 note -> 0.5, 1/4 -> 1,
// 1/8 -> 2, 1/16 -> 4, 1/32 -> 8). Bar is meter-dependent and handled by the
// caller; Off is never asked.
inline double SnapStepsPerBeat(SnapKind kind) {
    switch (kind) {
        case SnapKind::Half:         return 0.5;
        case SnapKind::Quarter:      return 1.0;
        case SnapKind::Eighth:       return 2.0;
        case SnapKind::Sixteenth:    return 4.0;
        case SnapKind::ThirtySecond: return 8.0;
        default:                     return 1.0;   // Bar / Off: unused
    }
}

// A bar's length in BEATS for the meter segment containing `f`: the governing
// MeterChange's `num` beats of `4/denom` quarter notes each, counted from that
// change's frame, matching TempoMap::BarBeat. Shared by SnapFrame and SnapStep
// so the grid line and the grid STEP cannot disagree about the bar.
inline double SnapBarBeats(const TempoMap& tm, Frame f) {
    double barBeats = 4.0;
    for (const MeterChange& m : tm.Meters()) {
        if (m.frame > f) break;
        const double denom = m.denom > 0 ? m.denom : 4;
        barBeats = (double)(m.num > 0 ? m.num : 4) * 4.0 / denom;
    }
    return barBeats > 0.0 ? barBeats : 4.0;
}

// Snap a timeline frame to the nearest grid line. `Off` and a Shift-held edit
// are the caller's business -- this returns `f` unchanged when the grid is off.
inline Frame SnapFrame(const TempoMap& tm, Frame f, SnapGrid grid) {
    if (!grid.On()) return f;
    if (f < 0) f = 0;

    const double beat = tm.BeatAt(f);

    if (grid.kind == SnapKind::Bar) {
        // The meter segment containing this frame, and the bar length in beats.
        double segBeat = 0.0;
        for (const MeterChange& m : tm.Meters()) {
            if (m.frame > f) break;
            segBeat = tm.BeatAt(m.frame);
        }
        const double barBeats = SnapBarBeats(tm, f);
        const double bars  = std::floor((beat - segBeat) / barBeats + 0.5);
        const Frame  out   = tm.FrameAt(segBeat + bars * barBeats);
        return out < 0 ? 0 : out;
    }

    double steps = SnapStepsPerBeat(grid.kind);
    if (grid.triplet) steps *= 1.5;      // 3 in the space of 2
    if (steps <= 0.0) return f;
    const double snapped = std::floor(beat * steps + 0.5) / steps;
    const Frame  out     = tm.FrameAt(snapped);
    return out < 0 ? 0 : out;
}

// The frame distance between two adjacent grid lines near `at` — what the
// keyboard's nudge (`,` `.`) moves a clip by. `Off` nudges by a 1/16 note (the
// editor's historical step), so a nudge always moves by something musical
// rather than by a single frame nobody can see.
inline Frame SnapStep(const TempoMap& tm, Frame at, SnapGrid grid) {
    if (at < 0) at = 0;
    const SnapGrid g = grid.On() ? grid
                                 : SnapGrid{ SnapKind::Sixteenth, false };
    const double fpb = tm.FramesPerBeatAt(at);
    double beats;
    if (g.kind == SnapKind::Bar) {
        beats = SnapBarBeats(tm, at);
    } else {
        double steps = SnapStepsPerBeat(g.kind);
        if (g.triplet) steps *= 1.5;
        beats = steps > 0.0 ? 1.0 / steps : 1.0;
    }
    Frame step = (Frame)(fpb * beats + 0.5);
    if (step < 1) step = 1;
    return step;
}

// The label the button and the tooltip show. "1/8T" for a triplet, "Off" for no
// snapping at all -- the indicator IS this string, so the menu entry and the
// button cannot disagree about what is selected. Indexed by SnapKind, so the
// two tables must stay in enum order (a host test walks every value).
inline const char* SnapGridLabel(SnapGrid grid) {
    static const char* const kPlain[] = { "Off", "Bar", "1/2", "1/4", "1/8",
                                          "1/16", "1/32" };
    static const char* const kTriplet[] = { "Off", "BarT", "1/2T", "1/4T", "1/8T",
                                            "1/16T", "1/32T" };
    const int i = (int)grid.kind;
    if (i < 0 || i > (int)SnapKind::ThirtySecond) return "Off";
    return grid.triplet ? kTriplet[i] : kPlain[i];
}

// The plain note values the menu offers, in the plan's order (bar, 1/2 … 1/32).
// Triplets and Off are modifiers, not values, and the view adds them as their
// own entries.
struct SnapDivision { const char* label; SnapKind kind; };
inline const SnapDivision* SnapDivisions(int* countOut) {
    static const SnapDivision kDivisions[] = {
        { "Bar",  SnapKind::Bar          },
        { "1/2",  SnapKind::Half         },
        { "1/4",  SnapKind::Quarter      },
        { "1/8",  SnapKind::Eighth       },
        { "1/16", SnapKind::Sixteenth    },
        { "1/32", SnapKind::ThirtySecond },
    };
    if (countOut) *countOut = (int)(sizeof(kDivisions) / sizeof(kDivisions[0]));
    return kDivisions;
}

} // namespace daw
