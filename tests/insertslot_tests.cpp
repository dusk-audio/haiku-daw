// Host-buildable tests for RunInsertSlot — the one definition of what an FX
// insert does, shared by the RT engine and the offline Exporter.
//
// The render tests (fx_insert_render_tests) prove the OFFLINE result end to end.
// These prove the function itself, block by block, including the two things a
// whole-render test structurally cannot reach:
//   - the state a delay line carries ACROSS blocks, and
//   - a bypass toggle DURING playback, which offline can never do because slot
//     state is fixed for the whole render.
// That second one is the live-only behavior the engine exists to get right, and
// until this function was shared it was untestable on any host.

#include "../src/engine/InsertSlot.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// An effect with a settable reported latency that actually delays by that much,
// so "reports N and really delays N" can be tested rather than assumed. Its wet
// signal is also scaled, so wet and dry are trivially distinguishable.
class DelayGainEffect : public IEffect {
public:
    DelayGainEffect(int latency, float gain) : fLat(latency), fGain(gain) {}
    void Prepare(double) override { fRing.assign((size_t)fLat * 2, 0.0f); fW = 0; }
    void Process(float* s, int frames) override {
        fProcessCalls++;
        for (int i = 0; i < frames; i++) {
            float l = s[i * 2 + 0], r = s[i * 2 + 1];
            if (fLat > 0) {                     // real N-frame delay
                const float ol = fRing[fW * 2 + 0], orr = fRing[fW * 2 + 1];
                fRing[fW * 2 + 0] = l;
                fRing[fW * 2 + 1] = r;
                l = ol; r = orr;
                if (++fW >= (size_t)fLat) fW = 0;
            }
            s[i * 2 + 0] = l * fGain;
            s[i * 2 + 1] = r * fGain;
        }
    }
    void Reset() override {}
    int LatencySamples() const override { return fLat; }
    const char* Name() const override { return "DelayGain"; }

    int    fProcessCalls = 0;
private:
    int                fLat;
    float              fGain;
    std::vector<float> fRing;
    size_t             fW = 0;
};

// A ramp so every frame is distinguishable and a misalignment of even one frame
// shows up as a value mismatch rather than silence-vs-silence.
static std::vector<float> Ramp(int frames, float base) {
    std::vector<float> v((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        v[i * 2 + 0] = base + (float)i;
        v[i * 2 + 1] = -(base + (float)i);
    }
    return v;
}

int main() {
    const int kFrames = 8;

    // --- Zero-latency effect ---------------------------------------------
    {
        // Fully wet: a bare Process, output scaled by the effect.
        DelayGainEffect e(0, 2.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare(0);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(&e, d, false, 1.0f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++) CHECK(buf[i] == in[i] * 2.0f);
        CHECK(e.fProcessCalls == 1);
    }
    {
        // Bypassed, zero latency: untouched, and Process never runs at all.
        DelayGainEffect e(0, 2.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare(0);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(&e, d, true, 1.0f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++) CHECK(buf[i] == in[i]);
        CHECK(e.fProcessCalls == 0);
    }
    {
        // mix = 0.5, zero latency: exactly halfway between dry and wet.
        DelayGainEffect e(0, 3.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare(0);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(&e, d, false, 0.5f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++)
            CHECK(buf[i] == in[i] * 3.0f * 0.5f + in[i] * 0.5f);
    }
    {
        // mix = 0, zero latency: fully dry even though Process still ran.
        DelayGainEffect e(0, 3.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare(0);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(&e, d, false, 0.0f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++) CHECK(buf[i] == in[i]);
    }

    // --- A null insert is a no-op (an unavailable plugin's index-aligned hole)
    {
        FrameDelay d;  d.Prepare(0);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(nullptr, d, false, 0.5f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++) CHECK(buf[i] == in[i]);
    }

    // --- Latent effect: bypass must preserve the REAL delay ----------------
    // The whole point of soft bypass. A bypassed insert reporting N still has to
    // delay by N, or the graph (which keeps compensating for N) lands this path
    // N frames early.
    {
        const int N = 3;
        DelayGainEffect e(N, 2.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare((size_t)N);
        std::vector<float> dry(kFrames * 2, 0.0f);

        // Block 1: the first N frames out are the line's initial zeros, then the
        // input delayed by N.
        std::vector<float> b1 = Ramp(kFrames, 1.0f);
        const std::vector<float> in1 = b1;
        RunInsertSlot(&e, d, true, 1.0f, b1.data(), kFrames, dry.data());
        CHECK(e.fProcessCalls == 0);            // bypassed: never processed
        for (int i = 0; i < N; i++) {
            CHECK(b1[i * 2 + 0] == 0.0f);
            CHECK(b1[i * 2 + 1] == 0.0f);
        }
        for (int i = N; i < kFrames; i++) {
            CHECK(b1[i * 2 + 0] == in1[(i - N) * 2 + 0]);
            CHECK(b1[i * 2 + 1] == in1[(i - N) * 2 + 1]);
        }

        // Block 2: the delay's tail must CARRY ACROSS the block boundary — the
        // first N frames come from the end of block 1, not from zeros again.
        std::vector<float> b2 = Ramp(kFrames, 100.0f);
        const std::vector<float> in2 = b2;
        RunInsertSlot(&e, d, true, 1.0f, b2.data(), kFrames, dry.data());
        for (int i = 0; i < N; i++) {
            CHECK(b2[i * 2 + 0] == in1[(kFrames - N + i) * 2 + 0]);
            CHECK(b2[i * 2 + 1] == in1[(kFrames - N + i) * 2 + 1]);
        }
        for (int i = N; i < kFrames; i++)
            CHECK(b2[i * 2 + 0] == in2[(i - N) * 2 + 0]);
    }

    // --- Latent effect at mix = 0.5: the legs stay phase-aligned -----------
    // Both legs are delayed by N, so the output is the plain blend of the
    // DELAYED dry with the wet. An undelayed dry leg would comb-filter.
    {
        const int N = 3;
        DelayGainEffect e(N, 2.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare((size_t)N);
        std::vector<float> buf = Ramp(kFrames, 1.0f), dry(kFrames * 2, 0.0f);
        const std::vector<float> in = buf;
        RunInsertSlot(&e, d, false, 0.5f, buf.data(), kFrames, dry.data());
        for (int i = N; i < kFrames; i++) {
            const float delayedDry = in[(i - N) * 2 + 0];
            CHECK(buf[i * 2 + 0] == delayedDry * 2.0f * 0.5f + delayedDry * 0.5f);
        }
    }

    // --- THE LIVE-ONLY CASE: toggling bypass mid-stream --------------------
    // A latent insert runs fully wet for a while, then the user hits bypass. The
    // delay line must already hold the last N frames of dry signal, or the first
    // block after the toggle emits a stale/zero burst — the click the whole soft
    // bypass design exists to avoid. No offline render can reach this: its slot
    // state is fixed for the entire bounce.
    {
        const int N = 3;
        DelayGainEffect e(N, 2.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare((size_t)N);
        std::vector<float> dry(kFrames * 2, 0.0f);

        std::vector<float> b1 = Ramp(kFrames, 1.0f);   // fully wet block
        const std::vector<float> in1 = b1;
        RunInsertSlot(&e, d, false, 1.0f, b1.data(), kFrames, dry.data());
        CHECK(e.fProcessCalls == 1);

        std::vector<float> b2 = Ramp(kFrames, 100.0f); // now BYPASSED
        const std::vector<float> in2 = b2;
        RunInsertSlot(&e, d, true, 1.0f, b2.data(), kFrames, dry.data());

        // Continuity: the first N frames after the toggle are the LAST N frames
        // of the previous block's input, exactly as if the delay had been in
        // circuit all along. Zeros here would mean the ring went unclocked while
        // the insert was fully wet.
        for (int i = 0; i < N; i++) {
            CHECK(b2[i * 2 + 0] == in1[(kFrames - N + i) * 2 + 0]);
            CHECK(b2[i * 2 + 1] == in1[(kFrames - N + i) * 2 + 1]);
            CHECK(b2[i * 2 + 0] != 0.0f);   // guard against a vacuous pass
        }
        for (int i = N; i < kFrames; i++)
            CHECK(b2[i * 2 + 0] == in2[(i - N) * 2 + 0]);
    }

    // A non-finite sample must not be manufactured by the fully-wet ring
    // clocking: Push touches only the ring, so an Inf in the dry signal can
    // never surface as 0 * Inf = NaN in the output (which is what a level-0
    // ProcessAdd would have done).
    {
        const int N = 2;
        DelayGainEffect e(N, 1.0f);
        e.Prepare(48000.0);
        FrameDelay d;  d.Prepare((size_t)N);
        std::vector<float> buf(kFrames * 2, 1.0f), dry(kFrames * 2, 0.0f);
        buf[0] = std::numeric_limits<float>::infinity();
        RunInsertSlot(&e, d, false, 1.0f, buf.data(), kFrames, dry.data());
        for (size_t i = 0; i < buf.size(); i++) CHECK(!std::isnan(buf[i]));
    }

    std::printf("insertslot_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
