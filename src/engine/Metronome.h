// Metronome — a click generator for the transport.
//
// Renders short click ticks at each beat (an accented, higher tick on the bar
// downbeat) into an interleaved-stereo buffer. Like Synth, the output is a pure
// function of the block's start frame — phase and envelope come from the
// distance since the last beat boundary — so it carries no state and is safe to
// mix in the RT callback. Kit-free, host-testable.
#pragma once

#include "../model/types.h"

#include <cstddef>

namespace daw {

class Metronome {
public:
    Metronome(double sampleRate = 48000.0, double tempoBPM = 120.0,
              int beatsPerBar = 4)
        : fSampleRate(sampleRate), fTempoBPM(tempoBPM),
          fBeatsPerBar(beatsPerBar < 1 ? 1 : beatsPerBar) {}

    double FramesPerBeat() const { return fSampleRate * 60.0 / fTempoBPM; }

    // Add click ticks for [blockStart, blockStart+frames) into `out`, scaled
    // by `gain`. Never allocates.
    void Render(float* out, size_t frames, Frame blockStart, float gain) const;

private:
    double fSampleRate;
    double fTempoBPM;
    int    fBeatsPerBar;
};

} // namespace daw
