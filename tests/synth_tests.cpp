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

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
