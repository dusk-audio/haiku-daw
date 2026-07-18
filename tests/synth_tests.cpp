// Host-buildable tests for the Synth: a rendered note has energy during its
// span and silence outside it, contributions are additive, and block-split
// rendering matches whole-block rendering (deterministic phase).

#include "../src/synth/Synth.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static float rms(const std::vector<float>& b) {
    double s = 0;
    for (float v : b) s += (double)v * v;
    return b.empty() ? 0.0f : (float)std::sqrt(s / b.size());
}

int main() {
    const double SR = 48000.0;
    Synth synth(SR);

    // Baseline instrument: attack only, no decay/release (matches the old
    // sine behaviour so the determinism/silence checks stay exact).
    Instrument inst;
    inst.waveform = (int)Waveform::Sine;
    inst.attack = 0.005f; inst.decay = 0.0f; inst.sustain = 1.0f; inst.release = 0.0f;

    // One 0.1 s note (A4) at frame 0.
    std::vector<MidiNote> notes;
    MidiNote n; n.pitch = 69; n.velocity = 100; n.startFrame = 0;
    n.lengthFrames = 4800;
    notes.push_back(n);

    // During the note: energy present.
    {
        std::vector<float> buf(4800 * 2, 0.0f);
        synth.Render(notes, inst, buf.data(), 4800, 0, 1.0f);
        CHECK(rms(buf) > 0.01f);
    }
    // After the note (no release): silence.
    {
        std::vector<float> buf(4800 * 2, 0.0f);
        synth.Render(notes, inst, buf.data(), 4800, 4800, 1.0f);
        CHECK(rms(buf) < 1e-6f);
    }

    // Split rendering (two 2400 blocks) == whole 4800 block, sample for sample.
    {
        std::vector<float> whole(4800 * 2, 0.0f);
        synth.Render(notes, inst, whole.data(), 4800, 0, 1.0f);

        std::vector<float> split(4800 * 2, 0.0f);
        synth.Render(notes, inst, split.data(), 2400, 0, 1.0f);
        synth.Render(notes, inst, split.data() + 2400 * 2, 2400, 2400, 1.0f);

        bool same = true;
        for (size_t i = 0; i < whole.size(); i++)
            if (std::fabs(whole[i] - split[i]) > 1e-5f) same = false;
        CHECK(same);
    }

    // Two notes are additive (louder than one).
    {
        std::vector<float> one(2400 * 2, 0.0f);
        synth.Render(notes, inst, one.data(), 2400, 0, 1.0f);

        std::vector<MidiNote> two = notes;
        MidiNote m; m.pitch = 72; m.velocity = 100; m.startFrame = 0;
        m.lengthFrames = 4800; two.push_back(m);
        std::vector<float> both(2400 * 2, 0.0f);
        synth.Render(two, inst, both.data(), 2400, 0, 1.0f);

        CHECK(rms(both) > rms(one));
    }

    // Waveform matters: a square wave differs from a sine over the same note.
    {
        Instrument sq = inst; sq.waveform = (int)Waveform::Square;
        std::vector<float> sine(4800 * 2, 0.0f), square(4800 * 2, 0.0f);
        synth.Render(notes, inst, sine.data(), 4800, 0, 1.0f);
        synth.Render(notes, sq,   square.data(), 4800, 0, 1.0f);
        bool differ = false;
        for (size_t i = 0; i < sine.size(); i++)
            if (std::fabs(sine[i] - square[i]) > 1e-3f) differ = true;
        CHECK(differ);
        // Square holds |s|=amp; sine averages lower -> square has higher RMS.
        CHECK(rms(square) > rms(sine));
    }

    // ADSR: a release tail rings past note-off, then decays to silence.
    {
        Instrument env; env.waveform = (int)Waveform::Sine;
        env.attack = 0.001f; env.decay = 0.0f; env.sustain = 1.0f;
        env.release = 0.050f;   // 2400-frame tail
        // Block right after the note: should have energy (the release tail)...
        std::vector<float> tail(4800 * 2, 0.0f);
        synth.Render(notes, env, tail.data(), 4800, 4800, 1.0f);
        CHECK(rms(tail) > 1e-4f);
        // ...and be quieter in its second half than its first (decaying).
        auto half = [&](size_t a, size_t b) {
            double s = 0; for (size_t i = a; i < b; i++) s += (double)tail[i]*tail[i];
            return std::sqrt(s / (b - a));
        };
        CHECK(half(0, 2400 * 2) > half(2400 * 2, 4800 * 2));
        // Well past the tail: silent.
        std::vector<float> gone(2400 * 2, 0.0f);
        synth.Render(notes, env, gone.data(), 2400, 4800 + 3000, 1.0f);
        CHECK(rms(gone) < 1e-6f);
    }

    // Band-limiting: a high-pitched saw has (almost) no real harmonic below its
    // fundamental, so energy in a sub-fundamental band is pure aliasing. PolyBLEP
    // should keep it small. A naive saw would fold strong aliases down here.
    {
        // Goertzel single-bin magnitude at frequency f (mono, left channel).
        auto goertzel = [&](const std::vector<float>& buf, double f) {
            const double w = 2.0 * M_PI * f / SR;
            const double cr = 2.0 * std::cos(w);
            double s1 = 0, s2 = 0;
            const size_t N = buf.size() / 2;
            for (size_t i = 0; i < N; i++) {
                const double s0 = buf[i * 2] + cr * s1 - s2;
                s2 = s1; s1 = s0;
            }
            return std::sqrt(s1 * s1 + s2 * s2 - cr * s1 * s2) / N;
        };

        Instrument saw = inst; saw.waveform = (int)Waveform::Saw;
        saw.attack = 0.0f;   // steady tone, no ramp coloring the spectrum
        std::vector<MidiNote> hi;
        MidiNote h; h.pitch = 120; h.velocity = 100; h.startFrame = 0;
        h.lengthFrames = 8192; hi.push_back(h);   // f0 ~= 8372 Hz (< Nyquist)

        std::vector<float> buf(8192 * 2, 0.0f);
        synth.Render(hi, saw, buf.data(), 8192, 0, 1.0f);

        // Sum magnitude in a sub-fundamental band [500, 4000] Hz — all aliasing.
        double aliasE = 0;
        for (double f = 500; f <= 4000; f += 250) aliasE += goertzel(buf, f);
        // Fundamental is present for reference.
        const double fundE = goertzel(buf, 8372.0);
        CHECK(fundE > 1e-3);                 // the note actually sounds
        CHECK(aliasE < 0.1 * fundE);         // aliases well below the fundamental
    }

    // Ramped channel gain: rendering from `from` to `to` scales the identical
    // note by a linear (i+1)/frames glide, so a controller the caller samples
    // once per block de-zippers instead of stepping at the block seam.
    {
        const size_t N = 512;
        std::vector<float> flat(N * 2, 0.0f), ramp(N * 2, 0.0f);
        synth.Render(notes, inst, flat.data(), N, 0, 1.0f);
        synth.Render(notes, inst, ramp.data(), N, 0,
                     StereoGain{0.0f, 0.0f}, StereoGain{1.0f, 1.0f});
        float maxErr = 0.0f;
        for (size_t i = 0; i < N; i++) {
            const float a = (float)(i + 1) / (float)N;
            maxErr = std::max(maxErr, std::fabs(ramp[i * 2 + 0] - flat[i * 2 + 0] * a));
            maxErr = std::max(maxErr, std::fabs(ramp[i * 2 + 1] - flat[i * 2 + 1] * a));
        }
        CHECK(maxErr < 1e-5f);                       // exact linear glide
        // Reaches the target on the final frame, so the next block can start
        // there and the gain is continuous across the whole render.
        CHECK(std::fabs(ramp[(N - 1) * 2 + 0] - flat[(N - 1) * 2 + 0]) < 1e-5f);
        CHECK(rms(ramp) < rms(flat));                // and it really is quieter
    }

    // from == to is exactly the constant-gain path: a project where no controller
    // moves renders bit-identically to before smoothing existed.
    {
        const size_t N = 512;
        std::vector<float> constant(N * 2, 0.0f), same(N * 2, 0.0f);
        synth.Render(notes, inst, constant.data(), N, 0, 0.5f, 0.25f);
        synth.Render(notes, inst, same.data(), N, 0,
                     StereoGain{0.5f, 0.25f}, StereoGain{0.5f, 0.25f});
        float d = 0.0f;
        for (size_t i = 0; i < N * 2; i++)
            d = std::max(d, std::fabs(constant[i] - same[i]));
        CHECK(d == 0.0f);                            // bit-identical
    }

    // Continuity: two half-blocks chained (0->0.5 then 0.5->1) match one whole
    // block ramped 0->1 at the seam, which is what the engine/exporter rely on
    // when they carry the previous block's end gain into the next.
    {
        const size_t N = 512, H = N / 2;
        std::vector<float> whole(N * 2, 0.0f), split(N * 2, 0.0f);
        synth.Render(notes, inst, whole.data(), N, 0,
                     StereoGain{0.0f, 0.0f}, StereoGain{1.0f, 1.0f});
        synth.Render(notes, inst, split.data(), H, 0,
                     StereoGain{0.0f, 0.0f}, StereoGain{0.5f, 0.5f});
        synth.Render(notes, inst, split.data() + H * 2, H, (Frame)H,
                     StereoGain{0.5f, 0.5f}, StereoGain{1.0f, 1.0f});
        // The two schemes sample the same ramp on a half-frame offset, so allow a
        // one-step tolerance; the point is that no discontinuity appears at H.
        const float tol = 1.0f / (float)H;
        float d = 0.0f;
        for (size_t i = 0; i < N * 2; i++)
            d = std::max(d, std::fabs(whole[i] - split[i]));
        CHECK(d < tol);                              // no seam jump
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
