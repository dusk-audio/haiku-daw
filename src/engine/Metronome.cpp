#include "Metronome.h"

#include <cmath>

namespace daw {

void Metronome::Render(float* out, size_t frames, Frame blockStart,
                       float gain, int sound) const {
    if (frames == 0) return;
    const double sr = fMap.sampleRate;
    if (sr < 1.0) return;

    const double clickLen = 0.030 * sr;   // 30 ms tick
    // The three sounds (M3.5, the Preferences window's metronome sound). Each
    // is a base frequency per beat with an accent an interval above it for the
    // bar's first beat, plus one partial; `decay` shapes the same 30 ms
    // envelope, so a sound is a timbre and nothing else.
    double beatFreq = 1000.0;   // Hz, regular beat
    double barFreq  = 1500.0;   // Hz, bar downbeat (accent)
    double partial  = 0.0;      // second partial, as a ratio of the base
    double partialAmp = 0.0;
    double decay    = 2.0;      // envelope exponent
    switch (sound) {
        case 1:   // beep: lower and rounder, one octave harmonic
            beatFreq = 700.0; barFreq = 950.0;
            partial = 2.0; partialAmp = 0.30; decay = 2.0;
            break;
        case 2:   // wood block: higher, a bright knock that dies fast
            beatFreq = 1800.0; barFreq = 2400.0;
            partial = 1.6; partialAmp = 0.45; decay = 3.0;
            break;
        default:  // click (0), and anything an older file happens to say
            break;
    }

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
            // The timbre: the base tone, plus the sound's one partial. The
            // envelope exponent is the sound's decay (click/beep 2 = the
            // original ear-shaped one, wood block 3 = a shorter knock).
            double v = std::sin(2.0 * M_PI * freq * t / sr);
            if (partialAmp > 0.0)
                v += partialAmp * std::sin(2.0 * M_PI * freq * partial * t / sr);
            const float s = (float)(v * std::pow(env, decay)) * gain;
            const size_t i = (size_t)(g - blockStart);
            out[i * 2 + 0] += s;
            out[i * 2 + 1] += s;
        }
    }
}

} // namespace daw
