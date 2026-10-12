// Host-buildable tests for the kit-free meter maths (daw::MeterMath).
//
// What is checked, and why each one can fail:
//   - the dB scale: 0 dBFS is full scale, -60 dB is the bottom, silence is a
//     finite floor (never -inf) and NaN is silent;
//   - the peak hold: it jumps to a new peak, holds it, then falls in dB per
//     second and never below the live bar;
//   - the RMS window: a sine of known amplitude reads ~0.707*A, silence reads
//     0, and the window really is a window (the reading falls after the
//     window has passed the tone);
//   - the correlation: +1 for a mono signal, -1 for one channel inverted, ~0
//     for independent channels, 0 for silence (not a division by zero);
//   - the clip latch: it takes the level, it stays latched, Reset clears it.
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/meter_tests.cpp -o /tmp/meter && /tmp/meter

#include "../src/dsp/MeterMath.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Interleaved stereo sine, `amp` on both channels. `invertR` flips the right
// channel's polarity (correlation -1).
static std::vector<float> Stereo(int frames, double sr, double hz, double amp,
                                 bool invertR = false) {
    std::vector<float> v((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const double s = std::sin(2.0 * M_PI * hz * (double)i / sr);
        v[(size_t)i * 2 + 0] = (float)(amp * s);
        v[(size_t)i * 2 + 1] = (float)(amp * (invertR ? -s : s));
    }
    return v;
}

int main() {
    // --- the dB scale -----------------------------------------------------
    CHECK(std::fabs(LinToDb(1.0f) - 0.0f) < 1e-4f);          // 0 dBFS
    CHECK(std::fabs(LinToDb(0.5f) + 6.0206f) < 1e-3f);       // halving = -6 dB
    CHECK(LinToDb(0.0f) == kMeterSilenceDb);
    CHECK(LinToDb(-0.25f) == kMeterSilenceDb);
    CHECK(LinToDb(std::nanf("")) == kMeterSilenceDb);
    CHECK(DbToFrac(kMeterMinDb) == 0.0f);
    CHECK(DbToFrac(kMeterMinDb - 12.0f) == 0.0f);
    CHECK(DbToFrac(kMeterMaxDb) == 1.0f);
    CHECK(DbToFrac(kMeterMaxDb + 6.0f) == 1.0f);
    CHECK(std::fabs(DbToFrac(0.0f) - 60.0f / 66.0f) < 1e-6f);   // 0 dBFS
    CHECK(std::fabs(LinToFrac(1.0f) - DbToFrac(0.0f)) < 1e-6f);
    CHECK(LinToFrac(0.0f) == 0.0f);
    CHECK(LinToFrac(2.0f) > DbToFrac(0.0f));    // over 0 dBFS reads higher
    // Monotonic over the scale.
    {
        float prev = -1.0f;
        bool rising = true;
        for (float db = kMeterMinDb; db <= kMeterMaxDb; db += 1.0f) {
            const float f = DbToFrac(db);
            if (f < prev) rising = false;
            prev = f;
        }
        CHECK(rising);
    }
    CHECK(kMeterTicksDb[0] == 0.0f);
    CHECK(DbToFrac(kMeterTicksDb[0]) > DbToFrac(kMeterTicksDb[1]));

    // --- the peak hold ----------------------------------------------------
    {
        PeakHold h;
        h.Update(0.8f, 1000000);
        CHECK(h.Frac() == 0.8f);
        // Holds while the bar falls away...
        h.Update(0.2f, 1000000 + PeakHold::kHoldUs - 1000);
        CHECK(h.Frac() == 0.8f);
        // ...then falls at the decay rate: half a second of 20 dB/s is
        // 10/66 of the scale.
        const int64_t t0 = 1000000 + PeakHold::kHoldUs;
        h.Update(0.2f, t0 + 500000);
        const float expected = 0.8f - (float)(0.5 * 20.0 / 66.0);
        CHECK(std::fabs(h.Frac() - expected) < 0.01f);
        // Never under the live bar.
        h.Update(0.79f, t0 + 600000);
        CHECK(h.Frac() >= 0.79f);
        // A new peak re-arms the hold.
        h.Update(0.95f, t0 + 700000);
        CHECK(h.Frac() == 0.95f);
        h.Update(0.1f, t0 + 700000 + PeakHold::kHoldUs - 1000);
        CHECK(h.Frac() == 0.95f);
        // Long silence: it reaches the floor, never below it.
        h.Update(0.0f, t0 + 700000 + PeakHold::kHoldUs + 60000000);
        CHECK(h.Frac() == 0.0f);
        h.Reset();
        CHECK(h.Frac() == 0.0f);
        // The decay follows the clock, not the tick rate: one 0.5 s step and
        // ten 0.05 s steps land in the same place.
        PeakHold a, b;
        a.Update(1.0f, 0); b.Update(1.0f, 0);
        const int64_t holdEnd = PeakHold::kHoldUs;
        a.Update(0.0f, holdEnd + 500000);
        for (int i = 1; i <= 10; i++)
            b.Update(0.0f, holdEnd + 50000 * i);
        CHECK(std::fabs(a.Frac() - b.Frac()) < 0.005f);
    }

    // --- the RMS window ---------------------------------------------------
    {
        StereoMeter m;
        m.Prepare(48000.0, 0.3f);
        // Feed exactly the window's worth of a 0.5-amplitude sine.
        std::vector<float> s = Stereo(14400, 48000.0, 1000.0, 0.5);
        m.Process(s.data(), 14400);
        const float expected = 0.5f * (float)(1.0 / std::sqrt(2.0));
        CHECK(std::fabs(m.RmsL() - expected) < 0.01f);
        CHECK(std::fabs(m.RmsR() - expected) < 0.01f);
        CHECK(m.Correlation() > 0.99f);    // mono: perfectly in phase

        // The window slides: a window's worth of silence reads ~0.
        std::vector<float> z((size_t)14400 * 2, 0.0f);
        m.Process(z.data(), 14400);
        CHECK(m.RmsL() < 0.02f);
        CHECK(m.Correlation() == 0.0f);    // silence has no correlation

        // Block size does not change the answer: the same signal fed in 128-,
        // 512- and 2048-frame blocks reads the same RMS.
        float rms[3];
        const int blocks[3] = { 128, 512, 2048 };
        for (int b = 0; b < 3; b++) {
            StereoMeter bm;
            bm.Prepare(48000.0, 0.3f);
            std::vector<float> sig = Stereo(48000, 48000.0, 1000.0, 0.5);
            for (size_t pos = 0; pos + (size_t)blocks[b] <= 48000;
                 pos += (size_t)blocks[b])
                bm.Process(sig.data() + pos * 2, blocks[b]);
            rms[b] = bm.RmsL();
        }
        CHECK(std::fabs(rms[0] - rms[1]) < 0.01f);
        CHECK(std::fabs(rms[1] - rms[2]) < 0.01f);
        CHECK(std::fabs(rms[0] - expected) < 0.01f);

        // A window shorter than the buffer: the reading keeps up with a
        // 100 ms window (a discontinued tone leaves the reading quickly).
        StereoMeter fast;
        fast.Prepare(48000.0, 0.1f);
        std::vector<float> tone = Stereo(48000, 48000.0, 1000.0, 0.5);
        for (size_t pos = 0; pos + 512 <= 48000; pos += 512)
            fast.Process(tone.data() + pos * 2, 512);
        CHECK(fast.RmsL() > 0.3f);
        std::vector<float> quiet((size_t)9600 * 2, 0.0f);
        for (size_t pos = 0; pos + 512 <= 9600; pos += 512)
            fast.Process(quiet.data() + pos * 2, 512);
        CHECK(fast.RmsL() < 0.02f);
        CHECK(fast.WindowSamples() <= (long long)(0.1 * 48000) + 512);

        // Reset clears everything.
        StereoMeter r;
        r.Prepare(48000.0, 0.3f);
        std::vector<float> sig = Stereo(4800, 48000.0, 1000.0, 1.0);
        r.Process(sig.data(), 4800);
        CHECK(r.RmsL() > 0.5f);
        r.Reset();
        CHECK(r.RmsL() == 0.0f && r.RmsR() == 0.0f);
        CHECK(r.Correlation() == 0.0f && r.WindowSamples() == 0);
    }

    // --- the correlation --------------------------------------------------
    {
        StereoMeter m;
        m.Prepare(48000.0, 0.3f);
        std::vector<float> inv = Stereo(14400, 48000.0, 1000.0, 0.5, true);
        m.Process(inv.data(), 14400);
        CHECK(m.Correlation() < -0.99f);
        // The RMS is unaffected by polarity.
        CHECK(std::fabs(m.RmsL() - 0.5f * (float)(1.0 / std::sqrt(2.0))) < 0.01f);

        // Independent channels: an uncorrelated pair averages to ~0. The
        // window has to be long enough for the average to converge, so this
        // uses 10 s of a 500 Hz / 1700 Hz pair.
        StereoMeter u;
        u.Prepare(48000.0, 2.0f);
        std::vector<float> mix((size_t)48000 * 2);
        for (int i = 0; i < 48000; i++) {
            mix[(size_t)i * 2 + 0] = (float)(0.5 * std::sin(2.0 * M_PI * 500.0 * i / 48000.0));
            mix[(size_t)i * 2 + 1] = (float)(0.5 * std::sin(2.0 * M_PI * 1700.0 * i / 48000.0));
        }
        for (int rep = 0; rep < 10; rep++) u.Process(mix.data(), 48000);
        CHECK(std::fabs(u.Correlation()) < 0.05f);

        // A dead right channel has no correlation to report (the denominator
        // is zero): it must read 0, not NaN.
        StereoMeter one;
        one.Prepare(48000.0, 0.3f);
        std::vector<float> onlyL = Stereo(14400, 48000.0, 1000.0, 0.5);
        for (size_t i = 0; i < (size_t)14400; i++) onlyL[i * 2 + 1] = 0.0f;
        one.Process(onlyL.data(), 14400);
        CHECK(one.Correlation() == 0.0f);
        CHECK(one.RmsL() > 0.3f);
        CHECK(one.RmsR() == 0.0f);

        // Correlation of a hard-panned mono signal stays +1 (identical, not
        // "both non-zero"): a mono-summed master must not read as 0.
        StereoMeter mono;
        mono.Prepare(48000.0, 0.3f);
        std::vector<float> ms = Stereo(14400, 48000.0, 440.0, 0.25);
        mono.Process(ms.data(), 14400);
        CHECK(mono.Correlation() > 0.99f);
    }

    // --- the clip level and the latch -------------------------------------
    {
        CHECK(!ReachesClipLevel(0.99f));
        CHECK(ReachesClipLevel(1.0f));    // 0 dBFS is a clip
        CHECK(ReachesClipLevel(1.4f));    // so is anything past it
        CHECK(ReachesClipLevel(32767.0f / 32768.0f));   // the int16 ceiling
        CHECK(!ReachesClipLevel(32766.0f / 32768.0f));  // one LSB below
        CHECK(!ReachesClipLevel(0.0f));
        CHECK(ReachesClipLevel(0.5f, 0.5f));   // the level is the caller's

        ClipLatch c;
        CHECK(!c.Clipped());
        c.Latch(ReachesClipLevel(0.99f));
        CHECK(!c.Clipped());
        c.Latch(ReachesClipLevel(1.0f));
        CHECK(c.Clipped());
        c.Latch(false);                   // stays latched
        CHECK(c.Clipped());
        c.Reset();
        CHECK(!c.Clipped());
        c.Latch(true);                    // a second clip re-latches
        CHECK(c.Clipped());
    }

    // The mixer strip's typed level entry (M3.4): what a user types into a dB
    // field, and what it means as a gain.
    {
        float db = 123.0f;
        CHECK(ParseDecibels("0", &db) && std::fabs(db - 0.0f) < 1e-4f);
        CHECK(ParseDecibels("-6", &db) && std::fabs(db + 6.0f) < 1e-4f);
        CHECK(ParseDecibels("+3.5", &db) && std::fabs(db - 3.5f) < 1e-4f);
        CHECK(ParseDecibels(" -12.25 ", &db) && std::fabs(db + 12.25f) < 1e-4f);
        // Silence: every spelling of -inf a user might reach for.
        CHECK(ParseDecibels("-inf", &db) && db == kFaderMinDb);
        CHECK(ParseDecibels("-INF", &db) && db == kFaderMinDb);
        CHECK(ParseDecibels("-oo", &db) && db == kFaderMinDb);
        CHECK(ParseDecibels("-99.9", &db) && db == kFaderMinDb);   // clamped
        // Clamped, not rejected: the fader's own range is the limit.
        CHECK(ParseDecibels("100", &db) && db == kFaderMaxDb);
        CHECK(ParseDecibels("-1000", &db) && db == kFaderMinDb);
        // Not a level: the caller keeps the old value.
        db = 42.0f;
        CHECK(!ParseDecibels("", &db));
        CHECK(!ParseDecibels("banana", &db));
        CHECK(!ParseDecibels("-6dB", &db));      // unit is implicit
        CHECK(!ParseDecibels("inf", &db));       // only a DOWNWARD inf
        CHECK(!ParseDecibels("6 7", &db));
        CHECK(!ParseDecibels("--6", &db));
        CHECK(db == 42.0f);                      // a refusal changes nothing

        // The gain conversion: 0 dB is unity, -6 dB is about a half, and the
        // floor is real silence (never a denormal-sized gain).
        CHECK(std::fabs(GainFromDb(0.0f) - 1.0f) < 1e-6f);
        CHECK(std::fabs(GainFromDb(-6.0206f) - 0.5f) < 1e-3f);
        CHECK(std::fabs(GainFromDb(6.0f) - 1.99526f) < 1e-3f);
        CHECK(GainFromDb(kFaderMinDb) == 0.0f);
        CHECK(GainFromDb(kFaderMinDb - 20.0f) == 0.0f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
