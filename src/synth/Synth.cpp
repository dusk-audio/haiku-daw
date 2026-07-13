#include "Synth.h"

#include <algorithm>
#include <cmath>

namespace daw {

// MIDI note number -> frequency in Hz (A4 = 69 = 440 Hz).
static double NoteFreq(int pitch) {
    return 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
}

void Synth::Render(const std::vector<MidiNote>& notes, float* out,
                   size_t frames, Frame blockStart, float gain) const {
    const double sr = fSampleRate;
    const double attack  = 0.005 * sr;   // 5 ms
    const double release = 0.020 * sr;   // 20 ms

    for (const MidiNote& n : notes) {
        const Frame noteEnd = n.startFrame + n.lengthFrames;
        // Skip notes that don't overlap this block at all.
        if (noteEnd <= blockStart
            || n.startFrame >= blockStart + (Frame)frames)
            continue;

        const double freq = NoteFreq(n.pitch);
        const double w = 2.0 * M_PI * freq / sr;
        const float  amp = (n.velocity / 127.0f) * 0.2f * gain;

        for (size_t i = 0; i < frames; i++) {
            const Frame g = blockStart + (Frame)i;
            if (g < n.startFrame || g >= noteEnd)
                continue;
            const double rel = (double)(g - n.startFrame);

            // Linear attack/release envelope.
            double env = 1.0;
            if (rel < attack)
                env = rel / attack;
            const double toEnd = (double)(noteEnd - g);
            if (toEnd < release)
                env = std::min(env, toEnd / release);

            const float s = (float)(std::sin(w * rel) * env) * amp;
            out[i * 2 + 0] += s;
            out[i * 2 + 1] += s;
        }
    }
}

} // namespace daw
