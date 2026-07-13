#include "Metronome.h"

#include <cmath>

namespace daw {

void Metronome::Render(float* out, size_t frames, Frame blockStart,
                       float gain) const {
    const double fpb = FramesPerBeat();
    if (fpb < 1.0)
        return;

    const double clickLen = 0.030 * fSampleRate;   // 30 ms tick
    const double beatFreq = 1000.0;                // Hz, regular beat
    const double barFreq  = 1500.0;                // Hz, bar downbeat (accent)

    for (size_t i = 0; i < frames; i++) {
        const double g = (double)(blockStart + (Frame)i);
        if (g < 0.0)
            continue;
        const long   beat = (long)std::floor(g / fpb);
        const double t    = g - (double)beat * fpb;   // frames since this beat
        if (t >= clickLen)
            continue;                                   // between ticks: silent

        const bool   isBar = (beat % fBeatsPerBar) == 0;
        const double freq  = isBar ? barFreq : beatFreq;
        const double env   = 1.0 - t / clickLen;        // linear decay
        const float  s     = (float)(std::sin(2.0 * M_PI * freq * t / fSampleRate)
                                     * env * env) * gain;
        out[i * 2 + 0] += s;
        out[i * 2 + 1] += s;
    }
}

} // namespace daw
