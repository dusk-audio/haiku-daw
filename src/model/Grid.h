// Grid — musical time math (bars / beats) derived from tempo + sample rate.
//
// The timeline stores frames; this converts to/from musical positions for the
// ruler and for snapping edits to the beat grid. Kit-free, header-only, so it
// builds and unit-tests anywhere. A "division" is a subdivision of a beat:
// 1 = beats, 2 = eighths, 4 = sixteenths.
#pragma once

#include "types.h"

#include <cmath>

namespace daw {

struct Grid {
    double sampleRate  = 48000.0;
    double tempoBPM    = 120.0;
    int    beatsPerBar = 4;

    double FramesPerBeat() const { return sampleRate * 60.0 / tempoBPM; }
    double FramesPerBar()  const { return FramesPerBeat() * beatsPerBar; }

    // Snap a frame to the nearest grid step (a beat / `divisionsPerBeat`).
    Frame Snap(Frame f, int divisionsPerBeat) const {
        if (divisionsPerBeat < 1) divisionsPerBeat = 1;
        const double step = FramesPerBeat() / divisionsPerBeat;
        if (step < 1.0) return f;
        Frame snapped = (Frame)(std::llround(f / step) * step);
        return snapped < 0 ? 0 : snapped;
    }

    // 1-indexed bar and beat for a frame (for the ruler readout).
    void BarBeat(Frame f, int* bar, int* beat) const {
        const double fpb = FramesPerBeat();
        const double fbar = FramesPerBar();
        if (f < 0) f = 0;
        const long b = (long)std::floor(f / fbar);
        const double within = f - b * fbar;
        *bar  = (int)b + 1;
        *beat = (int)std::floor(within / fpb) + 1;
    }
};

} // namespace daw
