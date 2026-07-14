#include "Synth.h"

#include <algorithm>
#include <cmath>

namespace daw {

// MIDI note number -> frequency in Hz (A4 = 69 = 440 Hz).
static double NoteFreq(int pitch) {
    return 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
}

// Naive (non-band-limited) oscillator for a phase in cycles (0..1 = one period).
static double Osc(int wave, double cycles) {
    const double p = cycles - std::floor(cycles);   // fractional phase [0,1)
    switch (wave) {
        case (int)Waveform::Saw:      return 2.0 * p - 1.0;
        case (int)Waveform::Square:   return p < 0.5 ? 1.0 : -1.0;
        case (int)Waveform::Triangle: return 2.0 * std::fabs(2.0 * p - 1.0) - 1.0;
        case (int)Waveform::Sine:
        default:                      return std::sin(2.0 * M_PI * p);
    }
}

// ADSR level at `rel` frames since note-on, for a note `noteLen` frames long.
// Attack ramp -> decay to sustain -> sustain hold; after note-off (rel>=noteLen)
// a release tail from whatever level the note held at off. All frame units.
static double Envelope(double rel, double noteLen,
                       double a, double d, double s, double r) {
    auto ads = [&](double x) -> double {   // attack/decay/sustain at x frames
        if (a > 0.0 && x < a)       return x / a;                 // 0 -> 1
        if (d > 0.0 && x < a + d)   return 1.0 - (1.0 - s) * (x - a) / d; // 1 -> s
        return s;                                                // sustain
    };
    if (rel < noteLen)
        return ads(rel);
    // Release tail from the level held at note-off.
    if (r <= 0.0) return 0.0;
    const double offLevel = ads(noteLen);
    const double t = (rel - noteLen) / r;   // 0 -> 1 across the release
    return t >= 1.0 ? 0.0 : offLevel * (1.0 - t);
}

void Synth::Render(const std::vector<MidiNote>& notes, const Instrument& inst,
                   float* out, size_t frames, Frame blockStart, float gain) const {
    const double sr = fSampleRate;
    const double a = inst.attack  * sr;
    const double d = inst.decay   * sr;
    const double s = std::clamp(inst.sustain, 0.0f, 1.0f);
    const double r = inst.release * sr;

    for (const MidiNote& n : notes) {
        // The note sounds from start through its release tail past note-off.
        const Frame noteEnd = n.startFrame + n.lengthFrames;
        const Frame soundEnd = noteEnd + (Frame)r;
        if (soundEnd <= blockStart || n.startFrame >= blockStart + (Frame)frames)
            continue;

        const double freq = NoteFreq(n.pitch);
        const double cyclesPerFrame = freq / sr;
        const float  amp = (n.velocity / 127.0f) * 0.2f * gain;
        const double noteLen = (double)n.lengthFrames;

        for (size_t i = 0; i < frames; i++) {
            const Frame g = blockStart + (Frame)i;
            if (g < n.startFrame || g >= soundEnd)
                continue;
            const double rel = (double)(g - n.startFrame);
            const double env = Envelope(rel, noteLen, a, d, s, r);
            if (env <= 0.0) continue;
            const float smp = (float)(Osc(inst.waveform, cyclesPerFrame * rel)
                                      * env) * amp;
            out[i * 2 + 0] += smp;
            out[i * 2 + 1] += smp;
        }
    }
}

} // namespace daw
