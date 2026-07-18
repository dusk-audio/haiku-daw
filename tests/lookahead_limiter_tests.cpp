// Host-buildable tests for the LIVE look-ahead limiter (dsp/LookaheadLimiter) —
// the first built-in effect that reports a non-zero IEffect::LatencySamples(),
// so it is what exercises Phase Y plugin-delay compensation on target.
//
// Verifies:
//   * LatencySamples() == the look-ahead window in frames, fixed at Prepare()
//     and unchanged by a live SetParam on the look-ahead slot (the PDC contract:
//     latency must be constant across a Prepare()/Process() lifetime).
//   * Below the ceiling the effect is a pure, sample-exact La-frame delay (unity
//     gain) — the exact-delay property PDC relies on to realign sibling paths.
//   * A loud signal (and a lone loud sample) is held at/under the ceiling.
//   * Reset() clears the delay tail so no stale audio bleeds across a seek.
//   * The EffectFactory builds it from a LimiterDesc and reports the same latency
//     (so the Engine/Exporter generic LatencySamples() sum picks it up).

#include "../src/dsp/LookaheadLimiter.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/model/Effect.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;
    const float  kCeilDb  = -1.0f;
    const float  kCeilLin = std::pow(10.0f, kCeilDb / 20.0f);   // ~0.8913

    // --- 1. Reported latency == round(lookaheadMs * SR / 1000), fixed. ---------
    {
        LookaheadLimiter lim(kCeilDb, 5.0, 60.0, 0.0);
        lim.Prepare(SR);
        CHECK(lim.LatencySamples() == 240);       // 5 ms @ 48 k

        // A live look-ahead change must NOT move the reported latency (would need
        // a realloc + break the PDC sizing done once at Load).
        lim.SetParam(1, 10.0f);
        CHECK(lim.LatencySamples() == 240);

        // Re-Prepare picks up the new look-ahead.
        lim.Prepare(SR);
        CHECK(lim.LatencySamples() == 480);       // 10 ms @ 48 k

        // Look-ahead always rounds up to at least one frame (a real, latent effect).
        LookaheadLimiter tiny(kCeilDb, 0.0, 60.0, 0.0);
        tiny.Prepare(SR);
        CHECK(tiny.LatencySamples() >= 1);
    }

    // --- 2. Below the ceiling: a pure, sample-exact La-frame delay at unity gain.
    {
        const int La = 240;
        const int N  = 2000;
        std::vector<float> in(N * 2);
        for (int i = 0; i < N; ++i) {
            const float s = 0.3f * std::sin(2.0f * float(M_PI) * 220.0f * i / SR);
            in[i * 2 + 0] = s;
            in[i * 2 + 1] = 0.5f * s;             // asymmetric L/R to catch swaps
        }
        std::vector<float> buf = in;
        LookaheadLimiter lim(kCeilDb, 5.0, 60.0, 0.0);
        lim.Prepare(SR);
        lim.Process(buf.data(), N);

        CHECK(lim.MeterDb() > -0.001f);           // no reduction below the ceiling

        // First La output frames are the latency lead-in (silence).
        float leadMax = 0.0f;
        for (int n = 0; n < La; ++n) {
            leadMax = std::max(leadMax, std::fabs(buf[n * 2 + 0]));
            leadMax = std::max(leadMax, std::fabs(buf[n * 2 + 1]));
        }
        CHECK(leadMax < 1e-6f);

        // out[n] == in[n - La] exactly (unity gain, exact integer delay).
        float delayErr = 0.0f;
        for (int n = La; n < N; ++n) {
            delayErr = std::max(delayErr,
                std::fabs(buf[n * 2 + 0] - in[(n - La) * 2 + 0]));
            delayErr = std::max(delayErr,
                std::fabs(buf[n * 2 + 1] - in[(n - La) * 2 + 1]));
        }
        CHECK(delayErr < 1e-6f);
    }

    // --- 3. A loud signal is held at/under the ceiling (with input drive). -----
    {
        const int N = 8000;
        std::vector<float> buf(N * 2);
        for (int i = 0; i < N; ++i) {
            const float s = 0.9f * std::sin(2.0f * float(M_PI) * 110.0f * i / SR);
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
        }
        // +12 dB input gain drives ~0.9 well over the -1 dB ceiling.
        LookaheadLimiter lim(kCeilDb, 5.0, 60.0, 12.0);
        lim.Prepare(SR);
        lim.Process(buf.data(), N);

        float outPk = 0.0f;
        for (float v : buf) outPk = std::max(outPk, std::fabs(v));
        CHECK(outPk <= kCeilLin + 1e-4f);         // ceiling held
        CHECK(outPk > 0.5f * kCeilLin);           // still passing limited signal
        CHECK(lim.MeterDb() < -1.0f);             // real gain reduction reported
    }

    // --- 4. A lone loud sample is caught (anticipated, held under ceiling). ----
    {
        const int La = 240, N = 3000, P = 1000;
        std::vector<float> buf(N * 2, 0.0f);
        buf[P * 2 + 0] = 4.0f;                    // one huge spike, L only
        buf[P * 2 + 1] = 4.0f;
        LookaheadLimiter lim(kCeilDb, 5.0, 60.0, 0.0);
        lim.Prepare(SR);
        lim.Process(buf.data(), N);

        // The spike emerges P+La later, attenuated to the ceiling.
        CHECK(std::fabs(buf[(P + La) * 2 + 0]) <= kCeilLin + 1e-4f);
        float outPk = 0.0f;
        for (float v : buf) outPk = std::max(outPk, std::fabs(v));
        CHECK(outPk <= kCeilLin + 1e-4f);
    }

    // --- 5. Reset() clears the delay tail (no bleed across a seek). ------------
    {
        const int N = 500;
        std::vector<float> buf(N * 2, 0.4f);      // fill the delay ring
        LookaheadLimiter lim(kCeilDb, 5.0, 60.0, 0.0);
        lim.Prepare(SR);
        lim.Process(buf.data(), N);
        lim.Reset();

        std::vector<float> sil(N * 2, 0.0f);
        lim.Process(sil.data(), N);
        float bleed = 0.0f;
        for (float v : sil) bleed = std::max(bleed, std::fabs(v));
        CHECK(bleed < 1e-6f);                     // silence in -> silence out
    }

    // --- 6. EffectFactory builds it and reports the same latency. --------------
    {
        auto e = MakeEffect(LimiterDesc(-1.0f, 5.0f, 60.0f, 6.0f));
        CHECK(e != nullptr);
        if (e) {
            CHECK(std::string(e->Name()) == "Limiter");
            e->Prepare(SR);
            CHECK(e->LatencySamples() == 240);    // generic Engine/Exporter sum sees this
        }
    }

    std::printf("lookahead_limiter_tests: %d checks, %d failures\n",
                g_checks, g_fails);
    return g_fails ? 1 : 0;
}
