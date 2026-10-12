#include "Synth.h"

#include <algorithm>
#include <cmath>

namespace daw {

// MIDI note number -> frequency in Hz (A4 = 69 = 440 Hz).
static double NoteFreq(int pitch) {
    return 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
}

// PolyBLEP residual: the correction added around a phase discontinuity to
// band-limit it (removes most of the aliasing a naive saw/square emits). `t`
// is the fractional phase [0,1); `dt` is the per-sample phase increment.
static double PolyBlep(double t, double dt) {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {                       // just after the step
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {                 // just before the next step
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

// Oscillator for a phase in cycles (0..1 = one period). `dt` = phase increment
// per sample (freq/sampleRate); saw and square are band-limited with PolyBLEP,
// so high notes no longer alias. Sine is inherently clean; triangle's aliasing
// is weak (harmonics fall 12 dB/oct) so it stays naive.
static double Osc(int wave, double cycles, double dt) {
    const double p = cycles - std::floor(cycles);   // fractional phase [0,1)
    switch (wave) {
        case (int)Waveform::Saw:
            return (2.0 * p - 1.0) - PolyBlep(p, dt);
        case (int)Waveform::Square: {
            double v = p < 0.5 ? 1.0 : -1.0;
            v += PolyBlep(p, dt);                    // rising edge at p=0
            double p2 = p + 0.5;                     // falling edge at p=0.5
            p2 -= std::floor(p2);
            v -= PolyBlep(p2, dt);
            return v;
        }
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
                   float* out, size_t frames, Frame blockStart,
                   StereoGain from, StereoGain to,
                   const VoiceExpression& expr) const {
    const double sr = fSampleRate;

    // Linear per-sample glide from `from` to `to`, reaching `to` exactly on the
    // last frame so the next block can start where this one ended (continuous).
    // A constant gain (the common case: no controller moved) skips the lerp.
    const bool  ramping = (from.l != to.l) || (from.r != to.r);
    const float step    = frames > 0 ? 1.0f / (float)frames : 0.0f;
    const float dL      = (to.l - from.l) * step;
    const float dR      = (to.r - from.r) * step;
    const double a = std::max(0.0f, inst.attack)  * sr;
    const double d = std::max(0.0f, inst.decay)   * sr;
    const double s = std::clamp(inst.sustain, 0.0f, 1.0f);
    const double r = std::max(0.0f, inst.release) * sr;

    // Expression is constant across the block (the caller evaluates it once per
    // block, like every other controller). Both terms default to "nothing to do"
    // so a track with no bend and no wheel runs exactly the loop it ran before
    // expression existed — bit for bit.
    const bool   bending  = (expr.bendRatio != 1.0f) || (expr.bendPhase != 0.0);
    const double bendStep = (double)expr.bendRatio - 1.0;
    const double vibDepth = (double)expr.modWheel;

    for (const MidiNote& n : notes) {
        // The note sounds from start through its release tail past note-off.
        const Frame noteEnd = n.startFrame + n.lengthFrames;
        const Frame soundEnd = noteEnd + (Frame)r;
        if (soundEnd <= blockStart || n.startFrame >= blockStart + (Frame)frames)
            continue;

        const double freq = NoteFreq(n.pitch);
        const double cyclesPerFrame = freq / sr;
        // Band-limiting sees the bend's ratio (constant over the block, so
        // exactly right); the vibrato's ±1 semitone wobble is small enough to
        // leave out of the PolyBLEP residual.
        const double dt = cyclesPerFrame * (double)expr.bendRatio;
        const float  amp = (n.velocity / 127.0f) * 0.2f;   // channel gain is per-side
        const double noteLen = (double)n.lengthFrames;
        // The bend's phase advance up to this block, minus the part that belongs
        // to the time before the note started: what is left is the offset the
        // note's own phase owes to bend, and it advances at the block's ratio.
        const double bendRel0 = expr.bendPhase - n.bendPhaseFrames;

        for (size_t i = 0; i < frames; i++) {
            const Frame g = blockStart + (Frame)i;
            if (g < n.startFrame || g >= soundEnd)
                continue;
            const double rel = (double)(g - n.startFrame);
            const double env = Envelope(rel, noteLen, a, d, s, r);
            if (env <= 0.0) continue;
            // Elapsed frames with the bend INTEGRATED (never a frequency
            // re-derived from the note, which would jump the phase the moment
            // the ratio stepped and click) plus the vibrato's integral. The
            // LFO's own phase runs on the note's real age, not the bent one.
            double elapsed = rel;
            if (bending) elapsed += bendRel0 + bendStep * (double)(g - blockStart);
            if (vibDepth > 0.0)
                elapsed += VibratoPhaseFrames(rel, sr, vibDepth);
            const float smp = (float)(Osc(inst.waveform, cyclesPerFrame * elapsed,
                                          dt) * env) * amp;
            const float gl = ramping ? (from.l + dL * (float)(i + 1)) : to.l;
            const float gr = ramping ? (from.r + dR * (float)(i + 1)) : to.r;
            out[i * 2 + 0] += smp * gl;
            out[i * 2 + 1] += smp * gr;
        }
    }
}

} // namespace daw
