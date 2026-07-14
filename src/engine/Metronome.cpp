#include "Metronome.h"

#include <cmath>

namespace daw {

void Metronome::Render(float* out, size_t frames, Frame blockStart,
                       float gain) const {
    if (frames == 0) return;
    const double sr = fMap.sampleRate;
    if (sr < 1.0) return;

    const double clickLen = 0.030 * sr;   // 30 ms tick
    const double beatFreq = 1000.0;       // Hz, regular beat
    const double barFreq  = 1500.0;       // Hz, bar downbeat (accent)

    const Frame blockEnd = blockStart + (Frame)frames;
    // Start from a beat that could still be ticking at the block start (a click
    // begun up to clickLen frames earlier tails into this block).
    Frame from = blockStart - (Frame)clickLen;
    if (from < 0) from = 0;
    long beat = (long)std::floor(fMap.BeatAt(from));
    if (beat < 0) beat = 0;

    for (;; beat++) {
        const Frame bf = fMap.FrameAt((double)beat);   // beat boundary frame
        if (bf >= blockEnd) break;

        int bar = 1, bb = 1;
        fMap.BarBeat(bf, &bar, &bb);
        const double freq = (bb == 1) ? barFreq : beatFreq;

        const long len = (long)clickLen;
        for (long k = 0; k < len; k++) {
            const Frame g = bf + (Frame)k;
            if (g < blockStart || g >= blockEnd) continue;
            const double t   = (double)k;
            const double env = 1.0 - t / clickLen;      // linear decay
            const float  s   = (float)(std::sin(2.0 * M_PI * freq * t / sr)
                                       * env * env) * gain;
            const size_t i = (size_t)(g - blockStart);
            out[i * 2 + 0] += s;
            out[i * 2 + 1] += s;
        }
    }
}

} // namespace daw
