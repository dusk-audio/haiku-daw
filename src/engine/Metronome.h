// Metronome — a click generator for the transport.
//
// Renders short click ticks at each beat (an accented, higher tick on the bar
// downbeat) into an interleaved-stereo buffer. Beat positions and accents come
// from a TempoMap, so it follows tempo and meter changes. Like Synth, the
// output is a pure function of the block's start frame (no state), so it is
// safe to mix in the RT callback. Kit-free, host-testable.
#pragma once

#include "../model/TempoMap.h"
#include "../model/types.h"

#include <cstddef>

namespace daw {

class Metronome {
public:
    Metronome(double sampleRate = 48000.0, double tempoBPM = 120.0,
              int beatsPerBar = 4) {
        fMap.sampleRate = sampleRate;
        fMap.Reset(tempoBPM, beatsPerBar, 4);
    }

    // Adopt a project's tempo/meter map (copied, so the RT thread owns it).
    void SetTempoMap(const TempoMap& m) { fMap = m; }

    double FramesPerBeat() const { return fMap.FramesPerBeatAt(0); }  // compat

    // Add click ticks for [blockStart, blockStart+frames) into `out`, scaled
    // by `gain`. Never allocates.
    void Render(float* out, size_t frames, Frame blockStart, float gain) const;

private:
    TempoMap fMap;
};

} // namespace daw
