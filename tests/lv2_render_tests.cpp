// LV2 inserts through the REAL render graph, bounced to WAV.
//
// Everything else that tests LV2 calls IEffect::Process directly. That proves
// the plugin wrapper, and nothing about whether an LV2 plugin actually survives
// the path a project takes: MakeEffect -> chain build -> RunInsertSlot ->
// Exporter -> WAV. This is the only test that covers that seam, and it uses the
// repo's own fixture bundle so it gives the same answer on every machine.
//
// The Exporter runs the same graph as the RT engine, so what holds here holds
// for playback too. Bounces are 32-bit float, hence quantisation- and
// dither-free, so the assertions are SAMPLE-EXACT rather than tolerances:
//
//   - a gain of 0.5 through the mono plugin must give exactly dry * 0.5. 0.5 is
//     a power of two, so the multiply is exact and "approximately" would be
//     hiding something.
//   - a plugin that reports 64 frames of latency AND really delays by 64 must
//     come back bit-identical to the dry render, because the exporter pads by
//     the graph's latency and trims it. That is plugin-delay compensation
//     working across the LV2 boundary, which nothing else exercises.

#include "../src/plugin/Lv2Host.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/model/Project.h"

#include <algorithm>
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

namespace {

const char* kMonoGain     = "urn:haiku-daw:test:mono-gain";
const char* kStereoLatent = "urn:haiku-daw:test:stereo-latent";
const char* kLatencyStep  = "urn:haiku-daw:test:latency-step";
const char* kSidechainGain = "urn:haiku-daw:test:sidechain-gain";
constexpr int kLatentFrames = 64;      // must match the fixture
constexpr int kStepFrames   = 128;     // latency-step's settled latency
const double SR = 48000.0;

void PutNote(Track& t, const MidiNote& n) {
    if (t.midiClips.empty()) { MidiClip c; c.id = 1; c.startFrame = 0;
                               c.lengthFrames = 1; t.midiClips.push_back(c); }
    MidiClip& c = t.midiClips.front();
    c.notes.push_back(n);
    const Frame end = n.startFrame + n.lengthFrames;
    if (end > c.lengthFrames) c.lengthFrames = end;
}

// Bounce one deterministic synth note through `fx`, returning interleaved
// stereo. Same shape as fx_insert_render_tests::Render, so the two agree on what
// "the graph" means.
std::vector<float> Render(const std::vector<EffectDesc>& fx) {
    Project p;
    p.sampleRate = SR;
    p.masterGain = 1.0f;

    Track t;
    t.id   = p.NextTrackId();
    t.type = TrackType::Midi;
    t.name = "synth";
    t.gain = 1.0f;
    t.pan  = 0.0f;
    MidiNote n;
    n.pitch        = 69;                            // A4
    n.velocity     = 110;
    n.startFrame   = 0;
    n.lengthFrames = static_cast<Frame>(SR / 4);    // 0.25 s
    PutNote(t, n);
    t.fx = fx;
    p.AddTrack(t);

    const std::string path = "/tmp/haiku_daw_lv2_render_test.wav";
    std::remove(path.c_str());
    std::vector<float> out;
    if (!ExportWav(p, path, SR, {ExportFormat{32}})) return out;
    WavSource src;
    if (!src.Open(path)) return out;
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames))
        out.insert(out.end(), chunk, chunk + frames * 2);
    std::remove(path.c_str());
    return out;
}

bool SameSamples(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    for (size_t i = 0; i < a.size(); i++) if (a[i] != b[i]) return false;
    return true;
}

// a[i] == b[i + shiftFrames] over every in-range frame.
bool SameShifted(const std::vector<float>& a, const std::vector<float>& b,
                 int shiftFrames) {
    if (a.empty() || b.empty()) return false;
    size_t compared = 0;
    for (size_t i = 0; i * 2 + 1 < a.size(); i++) {
        const long j = (long)i + shiftFrames;
        if (j < 0 || (size_t)j * 2 + 1 >= b.size()) continue;
        compared++;
        if (a[i * 2] != b[j * 2] || a[i * 2 + 1] != b[j * 2 + 1]) return false;
    }
    return compared > 1000;   // a shift overlapping almost nothing proves nothing
}

// A two-track project: `consumer` plays a full-length note and carries `fx` /
// `keyFx`, `key` plays the same note starting `keyAt` frames in. `keySrc` is
// what fx[0]'s sidechainSource is set to — the caller passes the KEY TRACK's id
// (the second one this helper allocates: a fresh Project hands out 1, then 2),
// or kInvalidTrackId for "no source", or the CONSUMER's own id to exercise the
// self-key rejection. `muteConsumer` takes the consumer's own material and
// plugin out of the mix entirely, which is how the key track's contribution is
// measured on its own.
constexpr TrackId kConsumerId = 1;   // p.NextTrackId() for the first track
constexpr TrackId kKeyTrackId = 2;   // ...and the second

std::vector<float> RenderKeyed(const std::vector<EffectDesc>& fx,
                               const std::vector<EffectDesc>& keyFx,
                               TrackId keySrc, bool muteConsumer, Frame keyAt) {
    Project p;
    p.sampleRate = SR;
    p.masterGain = 1.0f;

    MidiNote full;
    full.pitch        = 69;
    full.velocity     = 110;
    full.startFrame   = 0;
    full.lengthFrames = static_cast<Frame>(SR / 2);

    Track consumer;
    consumer.id   = p.NextTrackId();
    consumer.type = TrackType::Midi;
    consumer.name = "consumer";
    consumer.gain = 1.0f;
    consumer.pan  = 0.0f;
    PutNote(consumer, full);
    consumer.fx = fx;
    consumer.muted = muteConsumer;

    Track key;
    key.id   = p.NextTrackId();
    key.type = TrackType::Midi;
    key.name = "key";
    key.gain = 1.0f;
    key.pan  = 0.0f;
    key.fx = keyFx;
    MidiNote late = full;
    late.startFrame = keyAt;
    late.lengthFrames = static_cast<Frame>(SR / 2) - keyAt;
    PutNote(key, late);

    if (keySrc != kInvalidTrackId && !consumer.fx.empty())
        consumer.fx[0].sidechainSource = keySrc;

    p.AddTrack(consumer);
    p.AddTrack(key);

    const std::string path = "/tmp/haiku_daw_lv2_key_test.wav";
    std::remove(path.c_str());
    std::vector<float> out;
    if (!ExportWav(p, path, SR, {ExportFormat{32}})) return out;
    WavSource src;
    if (!src.Open(path)) return out;
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames))
        out.insert(out.end(), chunk, chunk + frames * 2);
    std::remove(path.c_str());
    return out;
}

// The largest |left| sample over frames [lo, hi).
float MaxAbs(const std::vector<float>& d, size_t lo, size_t hi) {
    float m = 0.0f;
    if (hi > d.size() / 2) hi = d.size() / 2;
    for (size_t i = lo; i < hi; i++) m = std::max(m, std::fabs(d[i * 2]));
    return m;
}

EffectDesc Lv2Desc(const char* uri, std::vector<float> params) {
    EffectDesc d;
    d.type       = EffectType::Lv2;
    d.pluginName = uri;
    d.params     = std::move(params);
    return d;
}

} // namespace

int main() {
    Lv2Host& host = Lv2Host::Instance();
    host.ScanAll();                       // also installs the EffectFactory hook

    if (!host.Find(kMonoGain) || !host.Find(kStereoLatent)) {
        std::printf("lv2_render_tests: fixture bundle not found "
                    "(LV2_PATH not set?) - skipping\n");
        std::printf("lv2_render_tests: 0 checks, 0 failures\n");
        return 0;
    }

    const std::vector<float> dry = Render({});
    CHECK(!dry.empty());
    if (dry.empty()) {
        std::printf("lv2_render_tests: dry render failed\n");
        return 1;
    }

    // --- A plugin's gain lands in the bounce, exactly ---------------------
    // Proves the whole seam: the factory resolved the URI, the chain built the
    // effect, RunInsertSlot ran it fully wet, and its output reached the file.
    {
        const std::vector<float> wet = Render({ Lv2Desc(kMonoGain, { 0.5f }) });
        CHECK(!wet.empty());
        CHECK(wet.size() == dry.size());
        if (!wet.empty() && wet.size() == dry.size()) {
            bool exact = true;
            for (size_t i = 0; i < dry.size(); i++)
                exact &= (wet[i] == dry[i] * 0.5f);
            CHECK(exact);
            // Not vacuous: the plugin really did something.
            CHECK(!SameSamples(wet, dry));
        }
    }

    // A mono plugin runs as TWO instances, one per channel. If they were cross-
    // wired both channels would carry the same signal; the synth's L and R
    // differ, so a bounce that came back with L == R would prove that bug.
    {
        const std::vector<float> wet = Render({ Lv2Desc(kMonoGain, { 1.0f }) });
        CHECK(!wet.empty());
        bool channelsDiffer = false;
        for (size_t i = 0; i + 1 < wet.size(); i += 2)
            if (wet[i] != wet[i + 1]) { channelsDiffer = true; break; }
        // Only meaningful if the DRY render has distinct channels to begin with.
        bool dryDiffers = false;
        for (size_t i = 0; i + 1 < dry.size(); i += 2)
            if (dry[i] != dry[i + 1]) { dryDiffers = true; break; }
        if (dryDiffers) CHECK(channelsDiffer);
        CHECK(SameSamples(wet, dry));      // unity gain is fully transparent
    }

    // --- Plugin-delay compensation across the LV2 boundary -----------------
    // The plugin reports 64 frames and really delays 64. The exporter pads the
    // render by the graph's latency and trims it, so a correctly compensated
    // latent insert lands back on the timeline bit-identical to dry. Assert the
    // shifted comparison FAILS too, so the test cannot pass by accident if the
    // delay were silently dropped or double-counted.
    {
        const std::vector<float> wet =
            Render({ Lv2Desc(kStereoLatent, { 1.0f, 0.0f }) });
        CHECK(!wet.empty());
        if (!wet.empty()) {
            CHECK(SameSamples(wet, dry));                    // aligned
            CHECK(!SameShifted(wet, dry, kLatentFrames));    // and NOT shifted
        }
    }

    // A latent insert that is soft-bypassed must still land aligned: bypass
    // keeps reporting its latency, and the host routes the signal through a
    // matching delay so the real delay is preserved too.
    {
        EffectDesc d = Lv2Desc(kStereoLatent, { 1.0f, 0.0f });
        d.bypassed = true;
        const std::vector<float> wet = Render({ d });
        CHECK(!wet.empty());
        if (!wet.empty()) CHECK(SameSamples(wet, dry));
    }

    // --- A latency that MOVES, settled before anything is sized (M4.6) -----
    // `latency-step` reports and applies 0 on its first run and 128 from its
    // second, so a host that read the port at activation -- or latched what it
    // read there -- would pad this insert by zero and render the whole bounce
    // 128 frames late (and cut that much off the tail). The exporter's
    // pre-roll settles it first, so the bounce is aligned bit-identically.
    {
        const std::vector<float> wet = Render({ Lv2Desc(kLatencyStep, { 1.0f }) });
        CHECK(!wet.empty());
        if (!wet.empty()) {
            CHECK(SameSamples(wet, dry));                    // aligned
            CHECK(!SameShifted(wet, dry, kStepFrames));      // and NOT shifted
        }
    }

    // --- The key reaches an LV2 plugin's sidechain ports (M7.1 tail) -------
    // `sidechain-gain` is out = in * key, so the key is measurable rather than
    // a matter of belief:
    //
    //   - KEYED: the consumer sounds through the whole render and the key only
    //     from `keyAt` on, so before keyAt the plugin multiplies by a ZERO
    //     key. The mix there is silence (the key track has not started), and
    //     from keyAt the bounce diverges from the key track's own signal —
    //     which is only possible if the key arrived, in the block it is used.
    //   - UNKEYED (no sidechain source): the insert's key buffers carry
    //     silence, so the CONSUMER's contribution is exactly zero — the whole
    //     bounce equals the key track rendered alone. That is the assertion
    //     that the ports are connected to the host's buffers rather than left
    //     at NULL or fed a stale key.
    {
        const Frame keyAt = static_cast<Frame>(SR / 4);
        EffectDesc sc = Lv2Desc(kSidechainGain, { 1.0f });

        const std::vector<float> keyed   = RenderKeyed({ sc }, {}, kKeyTrackId,
                                                       /*mute=*/false, keyAt);
        const std::vector<float> unkeyed = RenderKeyed({ sc }, {},
                                                       kInvalidTrackId,
                                                       /*mute=*/false, keyAt);
        const std::vector<float> keyOnly = RenderKeyed({ sc }, {},
                                                       kInvalidTrackId,
                                                       /*mute=*/true, keyAt);
        CHECK(!keyed.empty() && !unkeyed.empty() && !keyOnly.empty());
        if (!keyed.empty() && !unkeyed.empty() && !keyOnly.empty()) {
            // Silence before the key starts, in both cases: nothing is multiplied
            // by a key that has not arrived.
            CHECK(MaxAbs(keyed, 0, (size_t)keyAt - 8) < 1e-6f);
            CHECK(MaxAbs(unkeyed, 0, (size_t)keyAt - 8) < 1e-6f);
            // The key track really does sound once it starts (or the two
            // assertions above would pass vacuously).
            CHECK(MaxAbs(keyOnly, (size_t)keyAt + 64, (size_t)keyAt + 4096) > 1e-3f);
            // No key routed: the consumer's insert contributes exactly silence,
            // so the bounce IS the key track alone.
            CHECK(SameSamples(unkeyed, keyOnly));
            // With the key routed, the consumer's keyed output is in the mix —
            // it can only differ from `keyOnly` if the key reached the plugin.
            CHECK(!SameSamples(keyed, keyOnly));
        }
    }

    // An insert whose PLUGIN has no sidechain ports never gets a key, even when
    // its descriptor names a source. That is not cosmetic: a key edge raises
    // the consumer's input latency to the key path's, so a plugin that would
    // ignore the signal must not move the mix. The key track here carries a
    // LATENT insert precisely so that a wrongly routed key is visible — the
    // consumer's own material would shift by the key path's 128 frames.
    {
        const Frame keyAt = static_cast<Frame>(SR / 4);
        EffectDesc plain = Lv2Desc(kStereoLatent, { 1.0f, 0.0f });
        const std::vector<EffectDesc> keyChain = { Lv2Desc(kLatencyStep, { 1.0f }) };

        const std::vector<float> withSrc = RenderKeyed({ plain }, keyChain,
                                                       kKeyTrackId,
                                                       /*mute=*/false, keyAt);
        const std::vector<float> noSrc   = RenderKeyed({ plain }, keyChain,
                                                       kInvalidTrackId,
                                                       /*mute=*/false, keyAt);
        CHECK(!withSrc.empty() && !noSrc.empty());
        if (!withSrc.empty() && !noSrc.empty())
            CHECK(SameSamples(withSrc, noSrc));

        // A self-key names no usable source either (no order can read a node's
        // own output first), so it must not change the render.
        const std::vector<float> selfKey = RenderKeyed({ plain }, keyChain,
                                                       kConsumerId,
                                                       /*mute=*/false, keyAt);
        if (!selfKey.empty() && !noSrc.empty())
            CHECK(SameSamples(selfKey, noSrc));

        EffectDesc named = plain;
        named.sidechainSource = kKeyTrackId;
        CHECK(!EffectTakesSidechain(named));
        CHECK(EffectTakesSidechain(Lv2Desc(kSidechainGain, { 1.0f })));
        CHECK(!EffectTakesSidechain(Lv2Desc(kStereoLatent, { 1.0f })));
    }

    // --- A missing plugin degrades to a hole, not to a broken render -------
    {
        const std::vector<float> wet =
            Render({ Lv2Desc("urn:haiku-daw:test:not-installed", { 1.0f }) });
        CHECK(!wet.empty());
        CHECK(SameSamples(wet, dry));      // the slot is skipped, nothing else
    }

    std::printf("lv2_render_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
