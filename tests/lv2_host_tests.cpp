// Host tests for the real LV2 layer: scan, filter, instantiate, process.
//
// Built only when lilv was found (see DAW_LV2 in CMakeLists.txt), and written to
// PASS on a machine with no LV2 plugins installed — it prints a skip notice
// instead. CI machines vary, and a test that needs someone's home directory to
// be populated is a test that gets disabled.
//
// Where possible the assertions are INVARIANTS over whatever plugins happen to
// exist ("everything listed can be instantiated", "nothing rejected is listed")
// rather than facts about specific URIs, so they keep their teeth on a machine
// with a different plugin set than the one this was written on.

#include "../src/plugin/Lv2Host.h"
#include "../src/plugin/Lv2PortMap.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/model/Effect.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

namespace {

// A modest sine, so a plugin sees something musical rather than a step.
std::vector<float> MakeSignal(int frames) {
    std::vector<float> v((size_t)frames * 2, 0.0f);
    for (int i = 0; i < frames; i++) {
        const float s = 0.25f * std::sin(2.0f * 3.14159265f * 440.0f
                                         * (float)i / 48000.0f);
        v[(size_t)i * 2]     = s;
        v[(size_t)i * 2 + 1] = s * 0.8f;      // not identical, so an L/R swap shows
    }
    return v;
}

bool AllFinite(const std::vector<float>& v) {
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}

// A plugin that has gone unstable produces enormous values rather than NaN.
bool Bounded(const std::vector<float>& v, float limit = 100.0f) {
    for (float x : v) if (std::fabs(x) > limit) return false;
    return true;
}

} // namespace

int main() {
    Lv2Host& host = Lv2Host::Instance();
    host.ScanAll();

    const std::vector<Lv2PluginInfo>& plugins = host.Plugins();
    const std::vector<Lv2RejectInfo>& rejects = host.Rejected();

    std::printf("lv2_host_tests: %zu hostable plugin(s), %zu rejected\n",
                plugins.size(), rejects.size());
    for (const Lv2RejectInfo& r : rejects)
        std::printf("  skipped: %s (%s)\n", r.name.c_str(), r.reason.c_str());

    // --- Invariants that hold with or without plugins installed ----------

    // An unknown URI degrades exactly like an unavailable native add-on: null,
    // never a throw or a half-built effect.
    CHECK(host.Create("urn:definitely-not-a-plugin", 48000.0) == nullptr);
    CHECK(host.Find("urn:definitely-not-a-plugin") == nullptr);

    // A rejected plugin must be invisible to every lookup path, not merely
    // absent from the list. Offering one in the UI would mean instantiation
    // failing later, at the point where a user has already put it in a chain.
    for (const Lv2RejectInfo& r : rejects) {
        CHECK(host.Find(r.uri) == nullptr);
        CHECK(host.Create(r.uri, 48000.0) == nullptr);
    }

    // Listing entries are well-formed and unique: the URI is the persisted id,
    // so a duplicate or empty one would make a saved project ambiguous.
    for (size_t i = 0; i < plugins.size(); i++) {
        CHECK(!plugins[i].uri.empty());
        CHECK(!plugins[i].name.empty());        // falls back to the URI
        CHECK(host.Find(plugins[i].uri) == &plugins[i]);
        for (size_t j = i + 1; j < plugins.size(); j++)
            CHECK(plugins[i].uri != plugins[j].uri);
    }

    // --- Work item 4: persistence degrades softly ------------------------
    // A project saved with an LV2 insert, opened where that plugin is missing.
    // The factory must yield null so the engine keeps an index-aligned hole,
    // exactly as it does for a native add-on that isn't installed.
    {
        EffectDesc missing;
        missing.type       = EffectType::Lv2;
        missing.pluginName = "http://example.org/not-installed";
        missing.params     = { 0.5f, 0.25f };
        CHECK(MakeEffect(missing, 48000.0) == nullptr);

        // Same for a descriptor whose URI was lost entirely.
        EffectDesc empty;
        empty.type = EffectType::Lv2;
        CHECK(MakeEffect(empty, 48000.0) == nullptr);
    }

    if (plugins.empty()) {
        std::printf("lv2_host_tests: no hostable LV2 plugins installed - "
                    "skipping instantiation tests\n");
        std::printf("lv2_host_tests: %d checks, %d failures\n", g_checks, g_fails);
        return g_fails == 0 ? 0 : 1;
    }

    // --- Every listed plugin really instantiates -------------------------
    // This is the whole point of filtering at scan time: the listing is a
    // promise, and this is the test that the promise is kept.
    int altered = 0;
    for (const Lv2PluginInfo& info : plugins) {
        std::unique_ptr<IEffect> fx = host.Create(info.uri, 48000.0);
        CHECK(fx != nullptr);
        if (!fx) { std::printf("  could not create %s\n", info.name.c_str()); continue; }

        fx->Prepare(48000.0);
        CHECK(fx->Name() != nullptr);

        // Latency is latched, so it must not move under Reset() — the engine
        // calls Reset mid-playback on a seek, and every delay line sized from
        // this value (PDC, RunInsertSlot's dry delay) has already been built.
        const int latency = fx->LatencySamples();
        CHECK(latency >= 0);
        fx->Reset();
        CHECK(fx->LatencySamples() == latency);

        const int kFrames = 512;
        std::vector<float> buf = MakeSignal(kFrames);
        const std::vector<float> in = buf;
        fx->Process(buf.data(), kFrames);
        CHECK(fx->LatencySamples() == latency);     // nor under Process
        CHECK(AllFinite(buf));
        CHECK(Bounded(buf));
        if (buf != in) altered++;

        // Out-of-range param slots are ignored rather than corrupting memory:
        // a project saved against a different version of the plugin can carry
        // more params than the plugin now has ports.
        fx->SetParam(-1, 0.5f);
        fx->SetParam((int)info.params.size() + 100, 0.5f);
        fx->SetParam(0, 1.0f);
        const float nan = std::nanf("");
        fx->SetParam(0, nan);                        // must not reach the port
        fx->Process(buf.data(), kFrames);
        CHECK(AllFinite(buf));

        // Silence in must not produce a self-oscillating or NaN output.
        std::vector<float> quiet((size_t)kFrames * 2, 0.0f);
        fx->Process(quiet.data(), kFrames);
        CHECK(AllFinite(quiet));
        CHECK(Bounded(quiet));
    }

    // With a handful of plugins available, a completely transparent set is
    // implausible; with only one or two it is entirely possible (a flat EQ at
    // its defaults IS transparent), so this only asserts where it is meaningful.
    std::printf("lv2_host_tests: %d of %zu plugin(s) altered the signal\n",
                altered, plugins.size());
    if (plugins.size() >= 3) CHECK(altered >= 1);

    // --- The over-max block path -----------------------------------------
    // Engine::SetBufferFrames has no upper bound, so Process must handle a block
    // larger than kMaxLv2BlockFrames without allocating. Chunking is claimed to
    // be equivalent to calling Process once per chunk, so assert exactly that:
    // one big call must be bit-for-bit identical to the same input fed in the
    // chunk sizes the loop itself would choose. That proves the loop is CORRECT,
    // not merely that it fails to crash.
    {
        const Lv2PluginInfo& info = plugins.front();
        const int kBig = kMaxLv2BlockFrames * 2 + 1234;   // 3 chunks, ragged tail

        std::unique_ptr<IEffect> a = host.Create(info.uri, 48000.0);
        std::unique_ptr<IEffect> b = host.Create(info.uri, 48000.0);
        CHECK(a != nullptr && b != nullptr);
        if (a && b) {
            a->Prepare(48000.0);
            b->Prepare(48000.0);

            std::vector<float> whole = MakeSignal(kBig);
            std::vector<float> piece = whole;

            a->Process(whole.data(), kBig);

            int done = 0;
            while (done < kBig) {
                const int n = Lv2ChunkFrames(kBig - done);
                b->Process(piece.data() + (size_t)done * 2, n);
                done += n;
            }

            CHECK(AllFinite(whole));
            CHECK(Bounded(whole));
            CHECK(whole == piece);      // chunking == the equivalent call sequence

            // A block of exactly the maximum, and one frame past it, are the
            // boundary cases the loop's comparison could get wrong.
            for (int n : { kMaxLv2BlockFrames, kMaxLv2BlockFrames + 1 }) {
                std::unique_ptr<IEffect> e = host.Create(info.uri, 48000.0);
                CHECK(e != nullptr);
                if (!e) continue;
                e->Prepare(48000.0);
                std::vector<float> v = MakeSignal(n);
                e->Process(v.data(), n);
                CHECK(AllFinite(v));
                CHECK(Bounded(v));
            }
        }
    }

    // --- Degenerate calls -------------------------------------------------
    {
        std::unique_ptr<IEffect> fx = host.Create(plugins.front().uri, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            std::vector<float> buf = MakeSignal(64);
            fx->Process(buf.data(), 0);        // no frames
            fx->Process(buf.data(), -5);       // nonsense frame count
            fx->Process(nullptr, 64);          // no buffer
            CHECK(AllFinite(buf));

            // Prepare at a rate the plugin was not instantiated for
            // re-instantiates; Prepare(<=0) is ignored rather than tearing the
            // instance down, because MakeEffect's rate argument is defaulted and
            // some call sites never pass one.
            fx->Prepare(44100.0);
            fx->Process(buf.data(), 64);
            CHECK(AllFinite(buf));
            fx->Prepare(0.0);
            fx->Process(buf.data(), 64);
            CHECK(AllFinite(buf));
            fx->Prepare(-1.0);
            fx->Process(buf.data(), 64);
            CHECK(AllFinite(buf));
        }
    }

    // --- Through the factory, the way the engine builds a chain -----------
    {
        const Lv2PluginInfo& info = plugins.front();
        EffectDesc d;
        d.type       = EffectType::Lv2;
        d.pluginName = info.uri;
        for (const Lv2ParamInfo& p : info.params) d.params.push_back(p.def);

        std::unique_ptr<IEffect> fx = MakeEffect(d, 48000.0);
        CHECK(fx != nullptr);                 // the hook was installed by ScanAll
        if (fx) {
            fx->Prepare(48000.0);
            std::vector<float> buf = MakeSignal(256);
            fx->Process(buf.data(), 256);
            CHECK(AllFinite(buf));
        }

        // MakeEffect's rate argument is defaulted; the LV2 path must still
        // produce a usable effect (bound to a stand-in rate, re-instantiated by
        // Prepare) rather than null.
        std::unique_ptr<IEffect> noRate = MakeEffect(d);
        CHECK(noRate != nullptr);
        if (noRate) {
            noRate->Prepare(48000.0);
            std::vector<float> buf = MakeSignal(256);
            noRate->Process(buf.data(), 256);
            CHECK(AllFinite(buf));
        }
    }

    std::printf("lv2_host_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
