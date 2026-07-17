// Host-buildable tests for the offline look-ahead true-peak limiter
// (dsp/Limiter). Verifies: transparent below the ceiling; a loud signal is
// held at/under the dBTP ceiling (measured with the same BS.1770 true-peak
// detector the meter uses); inter-sample (true) peaks are caught, not just
// sample peaks; the look-ahead attack anticipates a transient; and the gain is
// stereo-linked (a peak in one channel attenuates both, preserving the image).

#include "../src/dsp/Limiter.h"
#include "../src/dsp/Loudness.h"

#include <cmath>
#include <cstdio>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// True peak (dBTP) of an interleaved-stereo buffer via the BS.1770 meter.
static float TruePeakDb(const std::vector<float>& buf, double sr) {
    Loudness m;
    m.Prepare(sr);
    m.SetIntegratedEnabled(false);
    m.Process(buf.data(), static_cast<int>(buf.size() / 2));
    return m.TruePeakDb();
}

static float SamplePeak(const std::vector<float>& buf) {
    float pk = 0.0f;
    for (float v : buf) pk = std::max(pk, std::fabs(v));
    return pk;
}

int main() {
    const double SR = 48000.0;
    const float  kCeilDb = -1.0f;
    const float  kCeilLin = std::pow(10.0f, kCeilDb / 20.0f);   // ~0.8913

    // --- 1. Transparent below the ceiling: a quiet sine is left essentially
    // unchanged (no gain reduction). ------------------------------------------
    {
        const int N = 4800;
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; ++i) {
            const float s = 0.2f * std::sin(2.0f * float(M_PI) * 440.0f * i / SR);
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
        }
        std::vector<float> before = buf;
        Limiter lim(kCeilDb, 2.0f, 60.0f);
        const float gr = lim.Process(buf.data(), N, SR);
        CHECK(gr < 0.01f);                               // no reduction
        float maxDelta = 0.0f;
        for (size_t i = 0; i < buf.size(); ++i)
            maxDelta = std::max(maxDelta, std::fabs(buf[i] - before[i]));
        CHECK(maxDelta < 1e-4f);                         // untouched
    }

    // --- 2. Loud signal held at/under the ceiling. A 15 kHz sine at unity
    // amplitude has real inter-sample overshoot, so this exercises the TRUE-peak
    // path (its true peak exceeds its sample peak of 1.0). ---------------------
    {
        const int N = 9600;
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; ++i) {
            const float s = 1.0f * std::sin(2.0f * float(M_PI) * 15000.0f * i / SR);
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
        }
        const float inTp = TruePeakDb(buf, SR);
        CHECK(inTp > kCeilDb);                           // starts over the ceiling
        CHECK(inTp > 0.0f);                              // inter-sample > sample (0 dBFS)

        Limiter lim(kCeilDb, 2.0f, 60.0f);
        const float gr = lim.Process(buf.data(), N, SR);
        CHECK(gr > 0.0f && std::isfinite(gr));           // some reduction, finite
        const float outTp = TruePeakDb(buf, SR);
        CHECK(outTp <= kCeilDb + 0.3f);                  // ceiling held (dBTP)
        CHECK(SamplePeak(buf) > 0.3f);                   // signal survives (not gated)
    }

    // --- 3. Transient anticipation (look-ahead attack): a low tone plus a loud
    // spike. Samples just BEFORE the spike must already be attenuated (the gain
    // dipped ahead of the peak), unlike samples far from it. ------------------
    {
        const int N = 8000;
        const int kSpike = 4000;
        std::vector<float> buf(N * 2, 0.0f);
        for (int i = 0; i < N; ++i) {
            const float tone = 0.25f * std::sin(2.0f * float(M_PI) * 300.0f * i / SR);
            buf[i * 2 + 0] = tone;
            buf[i * 2 + 1] = tone;
        }
        buf[kSpike * 2 + 0] += 4.0f;                     // huge transient
        buf[kSpike * 2 + 1] += 4.0f;

        std::vector<float> before = buf;
        Limiter lim(kCeilDb, 3.0f, 60.0f);               // 3 ms lookahead = 144 smp
        lim.Process(buf.data(), N, SR);

        // Tone envelope far from the spike is untouched (tone < ceiling).
        const int farIdx = 500;
        const float farRatio = std::fabs(buf[farIdx * 2] / before[farIdx * 2]);
        CHECK(std::fabs(farRatio - 1.0f) < 0.05f);

        // ~1 ms before the spike (well inside the 3 ms lookahead) the gain has
        // already started ducking, so the tone there is measurably attenuated.
        const int preIdx = kSpike - 48;                  // 1 ms earlier
        const float preRatio = std::fabs(buf[preIdx * 2] / before[preIdx * 2]);
        CHECK(preRatio < 0.9f);                          // pre-duck present
    }

    // --- 4. Stereo-linked: a peak that exists only in L must attenuate R by the
    // same gain (no image shift). --------------------------------------------
    {
        const int N = 4800;
        // Put the spike on a tone PEAK (500 Hz period = 96 samples; 2424 = a
        // quarter-period peak) so the R sample there is solidly non-zero — a
        // zero-crossing frame would make rRatio a 0/0 (division by ~0).
        const int kSpike = 2424;
        std::vector<float> buf(N * 2, 0.0f);
        for (int i = 0; i < N; ++i) {
            buf[i * 2 + 0] = 0.2f * std::sin(2.0f * float(M_PI) * 500.0f * i / SR);
            buf[i * 2 + 1] = 0.2f * std::sin(2.0f * float(M_PI) * 500.0f * i / SR);
        }
        buf[kSpike * 2 + 0] += 5.0f;                     // spike on LEFT only
        std::vector<float> before = buf;
        Limiter lim(kCeilDb, 2.0f, 60.0f);
        lim.Process(buf.data(), N, SR);
        // At the spike, R (which had no peak of its own) is still ducked, and by
        // the same factor as L's tone component would be — i.e. R != its input.
        const float rRatio = std::fabs(buf[kSpike * 2 + 1] / before[kSpike * 2 + 1]);
        CHECK(rRatio < 0.5f);                            // R linked to L's reduction
    }

    // --- 5. No-op guards: null / empty / zero-rate return 0 and don't crash. --
    {
        Limiter lim(kCeilDb, 2.0f, 60.0f);
        std::vector<float> buf(8, 0.5f);
        CHECK(lim.Process(nullptr, 4, SR) == 0.0f);
        CHECK(lim.Process(buf.data(), 0, SR) == 0.0f);
        CHECK(lim.Process(buf.data(), 4, 0.0) == 0.0f);
    }

    (void)kCeilLin;
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
