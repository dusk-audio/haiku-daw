// Host-buildable render tests for FX insert slots, driven through the offline
// Exporter (which runs the same graph as the RT engine, so what holds here is
// what the bounce and playback both do).
//
// Everything is bounced to 32-bit float WAV and compared SAMPLE-EXACTLY: the
// float path is quantization- and dither-free, so "behaves like the chain
// without this effect" is testable as bit-identity rather than as a tolerance.
//
// A note on the latency cases and PDC. The exporter renders into a buffer
// padded by the graph's total latency and then TRIMS that pad, so a latent
// effect is made transparent rather than shifting the bounce. Combined with
// soft bypass — which preserves the insert's real N-frame delay instead of
// dropping it — that means a correctly bypassed latent insert lands back on the
// timeline, bit-identical to the UNDELAYED dry render. It is the flam bug (a
// plain skip, which removes N frames of real delay while the pad is still
// trimmed) that shifts the track N frames EARLY. Each latency case therefore
// asserts both halves: equality at the aligned offset AND inequality at the
// shifted one, so the test cannot pass by accident under either behavior.

#include "../src/dsp/EffectFactory.h"
#include "../src/dsp/IEffect.h"
#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/model/Project.h"

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

static const double SR = 48000.0;

// Put an absolute-frame note onto a MIDI track via a single region at frame 0
// (relative == absolute), growing the region window to cover it. Same helper as
// exporter_tests, so no external audio file is needed.
static void PutNote(Track& t, const MidiNote& n) {
    if (t.midiClips.empty()) { MidiClip c; c.id = 1; c.startFrame = 0;
                               c.lengthFrames = 1; t.midiClips.push_back(c); }
    MidiClip& c = t.midiClips.front();
    c.notes.push_back(n);
    const Frame end = n.startFrame + n.lengthFrames;
    if (end > c.lengthFrames) c.lengthFrames = end;
}

// Bounce one MIDI note through `fx` (track chain) and `masterFx`, returning the
// rendered interleaved-stereo floats. The synth is deterministic, so two calls
// with the same chain give the same samples.
static std::vector<float> Render(const std::vector<EffectDesc>& fx,
                                 const std::vector<EffectDesc>& masterFx = {}) {
    Project p;
    p.sampleRate = SR;
    p.masterGain = 1.0f;
    p.masterFx   = masterFx;

    Track t;
    t.id   = p.NextTrackId();
    t.type = TrackType::Midi;
    t.name = "synth";
    t.gain = 1.0f;
    t.pan  = 0.0f;
    MidiNote n;
    n.pitch        = 69;                    // A4
    n.velocity     = 110;
    n.startFrame   = 0;
    n.lengthFrames = static_cast<Frame>(SR / 4);   // 0.25 s
    PutNote(t, n);
    t.fx = fx;
    p.AddTrack(t);

    const std::string path = "/tmp/haiku_daw_fx_insert_test.wav";
    std::remove(path.c_str());
    std::vector<float> out;
    if (!ExportWav(p, path, SR, {ExportFormat{32}}))
        return out;
    WavSource src;
    if (!src.Open(path)) return out;
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames))
        out.insert(out.end(), chunk, chunk + frames * 2);
    std::remove(path.c_str());
    return out;
}

// Bit-identical over the whole render.
static bool SameSamples(const std::vector<float>& a,
                        const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != b[i]) return false;
    return true;
}

// a[i] == b[i + shiftFrames] over every in-range frame (compares b SHIFTED).
static bool SameShifted(const std::vector<float>& a, const std::vector<float>& b,
                        int shiftFrames) {
    if (a.empty() || b.empty()) return false;
    size_t compared = 0;
    for (size_t i = 0; i * 2 + 1 < a.size(); i++) {
        const long j = (long)i + shiftFrames;
        if (j < 0 || (size_t)j * 2 + 1 >= b.size()) continue;
        compared++;
        if (a[i * 2] != b[j * 2] || a[i * 2 + 1] != b[j * 2 + 1]) return false;
    }
    return compared > 1000;   // a shift that overlaps almost nothing proves nothing
}

// a == dry*(1-mix) + wet*mix, sample-exactly. Float addition is commutative, and
// the scalings are the same ones the exporter applies, so this is exact rather
// than approximate.
static bool SameBlend(const std::vector<float>& a, const std::vector<float>& dry,
                      const std::vector<float>& wet, float mix) {
    if (a.size() != dry.size() || a.size() != wet.size() || a.empty())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i] != wet[i] * mix + dry[i] * (1.0f - mix)) return false;
    return true;
}

// Peak level, so a test can prove it isn't comparing two silent buffers.
static float Peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float s : v) { const float a = std::fabs(s); if (a > p) p = a; }
    return p;
}

int main() {
    // The reference: the same note with no effect at all.
    const std::vector<float> dry = Render({});
    CHECK(!dry.empty());
    CHECK(Peak(dry) > 0.01f);   // there is real signal to compare

    // --- Zero-latency effect (every built-in but the limiter) -------------
    {
        const EffectDesc sat = SaturatorDesc(/*drive=*/0.8f, /*mix=*/1.0f,
                                             /*trimDb=*/0.0f);
        const std::vector<float> wet = Render({ sat });
        CHECK(!wet.empty());
        // The effect must actually change the signal, or every check below
        // would pass vacuously.
        CHECK(!SameSamples(wet, dry));

        // Bypassed: bit-identical to the chain without it.
        EffectDesc byp = sat;
        byp.bypassed = true;
        CHECK(SameSamples(Render({ byp }), dry));

        // mix = 0: fully dry, likewise bit-identical.
        EffectDesc dry0 = sat;
        dry0.mix = 0.0f;
        CHECK(SameSamples(Render({ dry0 }), dry));

        // mix = 0.5: the hand-computed average of the dry and fully-wet renders.
        EffectDesc half = sat;
        half.mix = 0.5f;
        const std::vector<float> mixed = Render({ half });
        CHECK(SameBlend(mixed, dry, wet, 0.5f));
        CHECK(!SameSamples(mixed, dry));   // not a no-op
        CHECK(!SameSamples(mixed, wet));

        // mix = 1 is exactly the pre-feature path.
        EffectDesc full = sat;
        full.mix = 1.0f;
        CHECK(SameSamples(Render({ full }), wet));

        // Bypass wins over mix: a bypassed insert is dry whatever mix says.
        EffectDesc bypWet = sat;
        bypWet.bypassed = true;
        bypWet.mix      = 1.0f;
        CHECK(SameSamples(Render({ bypWet }), dry));
    }

    // --- Latent effect: the look-ahead limiter ----------------------------
    // The one built-in that reports non-zero latency, so it is what exercises
    // the dry-path delay in both the bypass and the wet/dry blend.
    int limiterLatency = 0;
    {
        auto e = MakeEffect(LimiterDesc(-1.0f, 5.0f, 60.0f, 6.0f));
        CHECK(e != nullptr);
        if (e) { e->Prepare(SR); limiterLatency = e->LatencySamples(); }
    }
    CHECK(limiterLatency > 0);   // otherwise these cases test nothing

    {
        // +6 dB in so the limiter is actually working, not passing through.
        const EffectDesc lim = LimiterDesc(-1.0f, 5.0f, 60.0f, 6.0f);
        const std::vector<float> wet = Render({ lim });
        CHECK(!wet.empty());
        CHECK(!SameSamples(wet, dry));

        // Bypassed. Soft bypass keeps the insert's REPORTED latency (so PDC
        // stays valid) and, through the delay-only path, its REAL latency too —
        // so the PDC trim lands it back exactly on the timeline.
        EffectDesc byp = lim;
        byp.bypassed = true;
        const std::vector<float> bypassed = Render({ byp });
        CHECK(SameSamples(bypassed, dry));
        // ...and it is NOT the early-by-N render a plain skip would produce.
        // Without this half, the check above could not tell the two apart.
        CHECK(!SameShifted(bypassed, dry, limiterLatency));

        // mix = 0.5 on a latent insert. Both legs are delayed by N (the wet leg
        // by the effect, the dry leg by the matching delay line) and the pad is
        // trimmed, so the result is the same plain blend as the zero-latency
        // case — no comb offset between the legs.
        EffectDesc half = lim;
        half.mix = 0.5f;
        const std::vector<float> mixed = Render({ half });
        CHECK(SameBlend(mixed, dry, wet, 0.5f));
        // If the dry leg were NOT delayed, the blend would use a dry that is N
        // frames early. Assert that render is a different thing, so the check
        // above is proved to discriminate.
        {
            std::vector<float> early(dry.size(), 0.0f);
            for (size_t i = 0; i + (size_t)limiterLatency < dry.size() / 2; i++) {
                early[i * 2]     = dry[(i + limiterLatency) * 2];
                early[i * 2 + 1] = dry[(i + limiterLatency) * 2 + 1];
            }
            CHECK(!SameBlend(mixed, early, wet, 0.5f));
        }

        // mix = 0 on a latent insert is the dry signal delayed by N — which,
        // after the PDC trim, is the dry render again.
        EffectDesc dry0 = lim;
        dry0.mix = 0.0f;
        const std::vector<float> mix0 = Render({ dry0 });
        CHECK(SameSamples(mix0, dry));
        CHECK(!SameShifted(mix0, dry, limiterLatency));
    }

    // --- The master chain takes the same path -----------------------------
    {
        const EffectDesc sat = SaturatorDesc(0.8f, 1.0f, 0.0f);
        const std::vector<float> masterWet = Render({}, { sat });
        CHECK(!masterWet.empty());
        CHECK(!SameSamples(masterWet, dry));

        EffectDesc byp = sat;
        byp.bypassed = true;
        CHECK(SameSamples(Render({}, { byp }), dry));

        EffectDesc half = sat;
        half.mix = 0.5f;
        CHECK(SameBlend(Render({}, { half }), dry, masterWet, 0.5f));

        // A latent master insert, bypassed: the master chain's latency is part
        // of the trimmed pad too, so the same alignment argument holds.
        EffectDesc mlim = LimiterDesc(-1.0f, 5.0f, 60.0f, 6.0f);
        mlim.bypassed = true;
        const std::vector<float> mbyp = Render({}, { mlim });
        CHECK(SameSamples(mbyp, dry));
        CHECK(!SameShifted(mbyp, dry, limiterLatency));
    }

    // --- A bypassed insert does not disturb its neighbours ----------------
    {
        const EffectDesc sat = SaturatorDesc(0.8f, 1.0f, 0.0f);
        const EffectDesc eq  = EqDesc();
        EffectDesc bypEq = eq;
        bypEq.bypassed = true;
        // [saturator, bypassed eq] must equal [saturator] alone.
        CHECK(SameSamples(Render({ sat, bypEq }), Render({ sat })));
        // ...and a bypassed LATENT insert between two live ones likewise, since
        // it still contributes its latency to the chain and its own delay.
        EffectDesc bypLim = LimiterDesc(-1.0f, 5.0f, 60.0f, 6.0f);
        bypLim.bypassed = true;
        CHECK(SameSamples(Render({ sat, bypLim, eq }), Render({ sat, eq })));
    }

    std::printf("fx_insert_render_tests: %d checks, %d failures\n",
                g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
