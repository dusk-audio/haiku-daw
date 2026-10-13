// Host-buildable tests for the Metronome: a tick has energy right after each
// beat boundary and silence between ticks, split rendering matches whole-block
// rendering (deterministic), and the bar downbeat differs from a plain beat.

#include "../src/engine/Metronome.h"
#include "../src/app/AppSettings.h"   // kMetronomeSoundCount (the one table)

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static float rms(const std::vector<float>& b, size_t from, size_t to) {
    double s = 0; size_t n = 0;
    for (size_t i = from; i < to && i < b.size(); i++) { s += (double)b[i]*b[i]; n++; }
    return n ? (float)std::sqrt(s / n) : 0.0f;
}

int main() {
    const double SR = 48000.0;
    Metronome m(SR, 120.0, 4);        // 24000 frames/beat
    CHECK(m.FramesPerBeat() == 24000.0);

    // Render one beat's worth from frame 0.
    std::vector<float> buf(24000 * 2, 0.0f);
    m.Render(buf.data(), 24000, 0, 1.0f);
    // Energy in the first ~30 ms (the tick), silence well after it.
    CHECK(rms(buf, 0, 1440 * 2) > 0.01f);              // first 30 ms: tick
    CHECK(rms(buf, 5000 * 2, 20000 * 2) < 1e-4f);      // mid-beat: silence

    // Split rendering == whole rendering (deterministic, stateless).
    {
        std::vector<float> whole(24000 * 2, 0.0f), split(24000 * 2, 0.0f);
        m.Render(whole.data(), 24000, 0, 1.0f);
        m.Render(split.data(), 12000, 0, 1.0f);
        m.Render(split.data() + 12000 * 2, 12000, 12000, 1.0f);
        bool same = true;
        for (size_t i = 0; i < whole.size(); i++)
            if (std::fabs(whole[i] - split[i]) > 1e-5f) same = false;
        CHECK(same);
    }

    // Downbeat (beat 0) tick differs from beat 1 tick (accent frequency).
    {
        std::vector<float> b0(1440 * 2, 0.0f), b1(1440 * 2, 0.0f);
        m.Render(b0.data(), 1440, 0, 1.0f);         // bar downbeat
        m.Render(b1.data(), 1440, 24000, 1.0f);     // beat 1
        bool differ = false;
        for (size_t i = 0; i < b0.size(); i++)
            if (std::fabs(b0[i] - b1[i]) > 1e-3f) differ = true;
        CHECK(differ);
    }

    // Follows a tempo map: after doubling the tempo at frame 96000, the beat
    // spacing halves (12000 frames), so there's a tick at 96000 and 108000.
    {
        TempoMap tm; tm.sampleRate = SR;
        tm.SetTempoAt(96000, 240.0);
        Metronome mm(SR); mm.SetTempoMap(tm);
        std::vector<float> buf(120000 * 2, 0.0f);
        mm.Render(buf.data(), 120000, 0, 1.0f);
        auto tickAt = [&](size_t f) {
            return rms(buf, f * 2, (f + 1440) * 2) > 0.01f;
        };
        CHECK(tickAt(96000));                    // beat 4 (tempo change point)
        CHECK(tickAt(108000));                   // beat 5, 12000 frames later
        CHECK(rms(buf, 102000 * 2, 107000 * 2) < 1e-4f);  // silence between
    }

    // The three click sounds (M3.5, the Preferences window): a timbre is all
    // they are -- the tick stays on its beat, the accent stays on bar 1, and
    // the sound changes what the tick is made of.
    {
        std::vector<std::vector<float>> ticks;
        for (int snd = 0; snd < kMetronomeSoundCount; snd++) {
            std::vector<float> b(24000 * 2, 0.0f);
            m.Render(b.data(), 24000, 0, 1.0f, snd);
            // Every sound ticks at beat 0, accents it, and is silent between.
            CHECK(rms(b, 0, 1440 * 2) > 0.01f);
            CHECK(rms(b, 5000 * 2, 20000 * 2) < 1e-4f);
            ticks.push_back(std::move(b));

            // ... and the accent differs from a plain beat, in every sound.
            std::vector<float> b1(1440 * 2, 0.0f);
            m.Render(b1.data(), 1440, 24000, 1.0f, snd);
            bool accentDiffers = false;
            for (size_t i = 0; i < b1.size(); i++)
                if (std::fabs(ticks.back()[i] - b1[i]) > 1e-3f)
                    accentDiffers = true;
            CHECK(accentDiffers);
        }
        // The sounds are actually different from one another (a menu whose
        // three entries all sound the same is a bug, not a preference).
        auto differ = [](const std::vector<float>& a,
                         const std::vector<float>& b) {
            for (size_t i = 0; i < a.size() && i < b.size(); i++)
                if (std::fabs(a[i] - b[i]) > 1e-3f) return true;
            return false;
        };
        // kMetronomeSoundCount is three today; a fourth sound would need pairs
        // added, not an out-of-bounds read (which is undefined behaviour, not a
        // failing check).
        CHECK(ticks.size() >= 3);
        if (ticks.size() >= 3) {
            CHECK(differ(ticks[0], ticks[1]));
            CHECK(differ(ticks[0], ticks[2]));
            CHECK(differ(ticks[1], ticks[2]));
        }

        // An index no build knows is the default click, not silence.
        std::vector<float> odd(1440 * 2, 0.0f);
        m.Render(odd.data(), 1440, 0, 1.0f, 99);
        bool sameAsClick = true;
        for (size_t i = 0; i < odd.size(); i++)
            if (std::fabs(odd[i] - ticks[0][i]) > 1e-6f) sameAsClick = false;
        CHECK(sameAsClick);

        // The level is the caller's gain, and it scales the tick linearly
        // (the Preferences window's metronome level rides on this).
        std::vector<float> half(24000 * 2, 0.0f);
        m.Render(half.data(), 24000, 0, 0.5f);
        bool halfOfClick = true;
        for (size_t i = 0; i < half.size(); i++)
            if (std::fabs(half[i] - ticks[0][i] * 0.5f) > 1e-5f)
                halfOfClick = false;
        CHECK(halfOfClick);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
