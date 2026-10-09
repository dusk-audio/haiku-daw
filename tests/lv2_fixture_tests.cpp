// Deterministic LV2 host tests, run against the fixture bundle this repo builds
// (tests/lv2fixture/). LV2_PATH is pointed at the build tree by CTest, so this
// test sees five plugins of exactly-known behaviour on every machine.
//
// lv2_host_tests is the complement: it runs against whatever is really installed
// and can therefore only assert INVARIANTS ("everything listed instantiates").
// This file asserts VALUES, which is what actually pins the behaviour down:
//
//   - MonoDual routing — the two-instance path, proven by giving L and R
//     DIFFERENT signals and checking each came back scaled independently. A host
//     that fed both instances channel 0 would pass every invariant test ever
//     written and fail here.
//   - Latency — 64 frames, reported AND really applied, so the latching logic is
//     finally exercised against a non-zero value. Every plugin installed on
//     either dev machine reports 0, which made all prior latency assertions
//     vacuously true.
//   - Rejection — a bad topology, an unsatisfiable required feature, and a
//     mandatory CV port, instead of depending on a specific broken bundle or a
//     particular sfizz install being present. The CV case had no real example
//     on any available machine and was previously unit-tested only.
//   - Param slot order, ranges and clamping, against a real bundle rather than a
//     hand-written port table.

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

const char* kMonoGain    = "urn:haiku-daw:test:mono-gain";
const char* kStereoLatent= "urn:haiku-daw:test:stereo-latent";
const char* kBadTopology = "urn:haiku-daw:test:bad-topology";
const char* kNeedsFeature= "urn:haiku-daw:test:needs-feature";
const char* kCvPort      = "urn:haiku-daw:test:cv-port";

constexpr int kLatentFrames = 64;   // must match the fixture

bool Near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// LV2_PATH also carries the system spec bundles (see CMakeLists: without them
// lilv has no class hierarchy to resolve against), so the machine's own plugins
// are visible here too. Every count below is therefore scoped to OUR five URIs,
// which keeps the assertions exact on any machine instead of coupling them to
// whatever happens to be installed.
bool IsFixtureUri(const std::string& uri) {
    return uri.rfind("urn:haiku-daw:test:", 0) == 0;
}

const Lv2RejectInfo* FindReject(const std::vector<Lv2RejectInfo>& v,
                                const std::string& uri) {
    for (const Lv2RejectInfo& r : v) if (r.uri == uri) return &r;
    return nullptr;
}

} // namespace

int main() {
    Lv2Host& host = Lv2Host::Instance();
    host.ScanAll();

    const std::vector<Lv2PluginInfo>& plugins = host.Plugins();
    const std::vector<Lv2RejectInfo>& rejects = host.Rejected();

    int mineHosted = 0, mineRejected = 0;
    for (const Lv2PluginInfo& p : plugins) if (IsFixtureUri(p.uri)) mineHosted++;
    for (const Lv2RejectInfo& r : rejects) if (IsFixtureUri(r.uri)) mineRejected++;

    std::printf("lv2_fixture_tests: %zu hostable / %zu rejected in the world; "
                "%d / %d of them are fixtures\n",
                plugins.size(), rejects.size(), mineHosted, mineRejected);

    // The bundle must be found at all. If LV2_PATH did not reach the build tree
    // every assertion below would vacuously "pass" on an empty world, so fail
    // loudly instead — this test is worthless if it silently tests nothing.
    if (mineHosted == 0 && mineRejected == 0) {
        std::printf("  FAIL fixture bundle not found (LV2_PATH not set?)\n");
        std::printf("lv2_fixture_tests: 1 checks, 1 failures\n");
        return 1;
    }

    // Exactly two of our five are hostable; the other three are refused.
    CHECK(mineHosted == 2);
    CHECK(mineRejected == 3);
    CHECK(host.Find(kMonoGain) != nullptr);
    CHECK(host.Find(kStereoLatent) != nullptr);

    // --- Rejection, deterministically ------------------------------------
    {
        CHECK(host.Find(kBadTopology) == nullptr);
        CHECK(host.Create(kBadTopology, 48000.0) == nullptr);
        const Lv2RejectInfo* r = FindReject(rejects, kBadTopology);
        CHECK(r != nullptr);
        if (r) {
            std::printf("  rejected %s: %s\n", r->name.c_str(), r->reason.c_str());
            CHECK(r->reason.find("topology") != std::string::npos);
            CHECK(r->reason.find("3 in") != std::string::npos);   // the real count
        }

        CHECK(host.Find(kNeedsFeature) == nullptr);
        CHECK(host.Create(kNeedsFeature, 48000.0) == nullptr);
        const Lv2RejectInfo* f = FindReject(rejects, kNeedsFeature);
        CHECK(f != nullptr);
        if (f) {
            std::printf("  rejected %s: %s\n", f->name.c_str(), f->reason.c_str());
            CHECK(f->reason.find("feature") != std::string::npos);
            CHECK(f->reason.find("worker") != std::string::npos);
        }

        // A clean 2-in/2-out plugin that still must be refused, because it has a
        // MANDATORY CV port. CV is audio-rate, so the inert buffer that
        // satisfies an atom port would be overrun by a whole block — there is no
        // correctly-sized buffer to offer, and hosting it anyway would be a
        // memory-safety bug in the plugin's address space. This path previously
        // had no real example anywhere and was covered only by a hand-written
        // port table in lv2_portmap_tests.
        CHECK(host.Find(kCvPort) == nullptr);
        CHECK(host.Create(kCvPort, 48000.0) == nullptr);
        const Lv2RejectInfo* c = FindReject(rejects, kCvPort);
        CHECK(c != nullptr);
        if (c) {
            std::printf("  rejected %s: %s\n", c->name.c_str(), c->reason.c_str());
            CHECK(c->reason.find("required port") != std::string::npos);
        }
    }

    // --- Param metadata: slot order, names, ranges, defaults --------------
    // Asserted against a real bundle, so a bug in the lilv-reading half (which
    // the pure port-map tests cannot see) shows up here.
    {
        const Lv2PluginInfo* p = host.Find(kStereoLatent);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->params.size() == 2);          // the OUTPUT control is not a param
            CHECK(!p->monoDual);
            if (p->params.size() == 2) {
                CHECK(p->params[0].name == "Gain");
                CHECK(Near(p->params[0].def, 1.0f));
                CHECK(Near(p->params[0].mn, 0.0f));
                CHECK(Near(p->params[0].mx, 4.0f));
                CHECK(!p->params[0].isInteger);      // Gain is continuous
                CHECK(p->params[1].name == "Extra");
                CHECK(Near(p->params[1].def, 0.0f));
                CHECK(Near(p->params[1].mn, 0.0f));
                CHECK(Near(p->params[1].mx, 1.0f));
                // lv2:toggled read off a real bundle, not a hand-written table.
                CHECK(p->params[1].isInteger);
            }
        }
        const Lv2PluginInfo* m = host.Find(kMonoGain);
        CHECK(m != nullptr);
        if (m) {
            CHECK(m->monoDual);                    // 1 in / 1 out -> two instances
            CHECK(m->params.size() == 1);
            if (m->params.size() == 1) CHECK(m->params[0].name == "Gain");
        }
    }

    // --- MonoDual: each instance must get ITS OWN channel -----------------
    // The whole point of the two-instance topology. L and R are given different
    // values, so a host that wired both instances to channel 0 (or shared one
    // scratch buffer between them) produces R == L and fails here. No invariant
    // test can catch that.
    {
        std::unique_ptr<IEffect> fx = host.Create(kMonoGain, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            fx->SetParam(0, 2.0f);                 // gain = 2

            const int N = 32;
            std::vector<float> buf((size_t)N * 2, 0.0f);
            for (int i = 0; i < N; i++) {
                buf[(size_t)i * 2]     = 1.0f;     // L
                buf[(size_t)i * 2 + 1] = 0.5f;     // R, deliberately different
            }
            fx->Process(buf.data(), N);

            bool ok = true;
            for (int i = 0; i < N; i++) {
                ok &= Near(buf[(size_t)i * 2],     2.0f);   // 1.0 * 2
                ok &= Near(buf[(size_t)i * 2 + 1], 1.0f);   // 0.5 * 2, NOT 2.0
            }
            CHECK(ok);
            if (!ok)
                std::printf("    got L=%.3f R=%.3f (want 2.000 / 1.000)\n",
                            buf[0], buf[1]);

            // A mono plugin adds no latency.
            CHECK(fx->LatencySamples() == 0);
        }
    }

    // --- Param clamping against the port's declared range ------------------
    {
        std::unique_ptr<IEffect> fx = host.Create(kMonoGain, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            fx->SetParam(0, 100.0f);               // way past lv2:maximum 4.0
            std::vector<float> buf(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 4.0f));             // clamped, not 100
            CHECK(Near(buf[1], 4.0f));

            fx->SetParam(0, -50.0f);               // past lv2:minimum 0.0
            buf.assign(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 0.0f));

            fx->SetParam(0, std::nanf(""));        // must never reach the port
            buf.assign(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(std::isfinite(buf[0]));
        }
    }

    // --- Latency: reported AND genuinely applied ---------------------------
    // The assertion that was vacuous everywhere until this fixture existed.
    {
        std::unique_ptr<IEffect> fx = host.Create(kStereoLatent, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            CHECK(fx->LatencySamples() == kLatentFrames);

            // Constant across Reset() — the engine calls it mid-playback on a
            // seek, and PDC plus RunInsertSlot's dry delay are already sized.
            // The fixture rewrites its latency port on EVERY run(), so a host
            // that re-read it would be caught here.
            fx->Reset();
            CHECK(fx->LatencySamples() == kLatentFrames);

            const int N = 256;
            std::vector<float> buf((size_t)N * 2, 0.0f);
            buf[0] = 1.0f;                         // impulse on L, frame 0
            buf[1] = 1.0f;                         // and on R
            fx->Process(buf.data(), N);
            CHECK(fx->LatencySamples() == kLatentFrames);

            // Silence until exactly the reported latency, impulse at it.
            bool quietBefore = true;
            for (int i = 0; i < kLatentFrames; i++)
                quietBefore &= Near(buf[(size_t)i * 2], 0.0f) &&
                               Near(buf[(size_t)i * 2 + 1], 0.0f);
            CHECK(quietBefore);
            CHECK(Near(buf[(size_t)kLatentFrames * 2],     1.0f));
            CHECK(Near(buf[(size_t)kLatentFrames * 2 + 1], 1.0f));
            // ...and nothing after it.
            bool quietAfter = true;
            for (int i = kLatentFrames + 1; i < N; i++)
                quietAfter &= Near(buf[(size_t)i * 2], 0.0f);
            CHECK(quietAfter);

            // THE assertion this fixture exists for. From its second run the
            // plugin reports 999 while still delaying 64. The host must keep
            // returning the value it latched at Prepare: a graph whose delay
            // lines were sized for 64 cannot survive the number moving under it,
            // and re-reading the port would misalign both soft bypass and the
            // wet/dry blend with nothing to notice.
            buf.assign(buf.size(), 0.0f);
            fx->Process(buf.data(), N);
            CHECK(fx->LatencySamples() == kLatentFrames);
            fx->Process(buf.data(), N);
            CHECK(fx->LatencySamples() == kLatentFrames);

            // Reset() mid-playback must not adopt it either.
            fx->Reset();
            CHECK(fx->LatencySamples() == kLatentFrames);
            fx->Process(buf.data(), N);
            CHECK(fx->LatencySamples() == kLatentFrames);
        }
    }

    // --- Chunking equals the equivalent call sequence, on known data -------
    // lv2_host_tests asserts this too, but only that two paths AGREE. Here the
    // plugin is a pure delay, so the expected output is known outright and a
    // chunk boundary that dropped or duplicated frames is visible directly.
    {
        const int kBig = kMaxLv2BlockFrames * 2 + 777;
        std::unique_ptr<IEffect> fx = host.Create(kStereoLatent, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            std::vector<float> buf((size_t)kBig * 2, 0.0f);
            for (int i = 0; i < kBig; i++) {       // ramp: every frame distinct
                buf[(size_t)i * 2]     = (float)i;
                buf[(size_t)i * 2 + 1] = (float)i;
            }
            fx->Process(buf.data(), kBig);         // chunks internally

            // A pure kLatentFrames delay: out[i] == in[i - 64] == i - 64.
            bool ok = true;
            for (int i = kLatentFrames; i < kBig; i++)
                ok &= Near(buf[(size_t)i * 2], (float)(i - kLatentFrames), 0.01f);
            CHECK(ok);
            if (!ok) {
                // Report the first mismatch: its index localises the bad chunk.
                for (int i = kLatentFrames; i < kBig; i++)
                    if (!Near(buf[(size_t)i * 2], (float)(i - kLatentFrames), 0.01f)) {
                        std::printf("    first bad frame %d: got %.1f want %.1f\n",
                                    i, buf[(size_t)i * 2], (float)(i - kLatentFrames));
                        break;
                    }
            }
        }
    }

    // --- A sample-rate change must not reset the user's parameters ---------
    // Prepare() at a new rate re-instantiates, because an LV2 instance is bound
    // to the rate it was created at. The control VALUES live in the host, not the
    // instance, and the new instance is reconnected to them — so every knob must
    // survive. If it did not, changing the audio device would silently reset
    // every LV2 plugin in the project to its defaults, which would look like data
    // loss and would be nobody's idea of a device change.
    {
        std::unique_ptr<IEffect> fx = host.Create(kMonoGain, 44100.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(44100.0);
            fx->SetParam(0, 3.0f);                 // gain = 3, not the default 1

            std::vector<float> buf(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 3.0f));

            fx->Prepare(48000.0);                  // different rate -> re-instantiate
            buf.assign(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 3.0f));             // still 3, NOT back to 1.0
            CHECK(Near(buf[1], 3.0f));

            // And again, to a rate it has already used.
            fx->Prepare(44100.0);
            buf.assign(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 3.0f));
        }
    }

    // --- Reset() must actually clear the plugin's internal state -----------
    // The engine calls Reset on a seek precisely so stale audio does not bleed
    // across the jump (see IEffect). For a delay line that is directly
    // observable: fill it, Reset, and the buffered signal must never emerge.
    // Nothing previously tested that Reset does anything at all.
    {
        std::unique_ptr<IEffect> fx = host.Create(kStereoLatent, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);

            // Push an impulse in, but stop short of the latency so it is still
            // sitting inside the plugin's delay line, unemitted.
            const int kPartial = kLatentFrames / 2;
            std::vector<float> buf((size_t)kPartial * 2, 0.0f);
            buf[0] = 1.0f;
            buf[1] = 1.0f;
            fx->Process(buf.data(), kPartial);

            fx->Reset();                           // the seek

            // Now run well past the latency with silence. A working Reset means
            // the buffered impulse is gone; without one it would surface here.
            std::vector<float> quiet((size_t)(kLatentFrames * 3) * 2, 0.0f);
            fx->Process(quiet.data(), kLatentFrames * 3);
            bool clean = true;
            for (float v : quiet) clean &= Near(v, 0.0f);
            CHECK(clean);
            if (!clean)
                std::printf("    stale audio survived Reset()\n");
        }
    }

    // --- Through MakeEffect, the way the engine builds a chain -------------
    // Confirms the factory applies stored params AFTER the hook returns, using a
    // value whose effect is directly observable.
    {
        EffectDesc d;
        d.type       = EffectType::Lv2;
        d.pluginName = kMonoGain;
        d.params     = { 3.0f };                   // gain = 3

        std::unique_ptr<IEffect> fx = MakeEffect(d, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            std::vector<float> buf(2, 1.0f);
            fx->Process(buf.data(), 1);
            CHECK(Near(buf[0], 3.0f));             // stored param really applied
            CHECK(Near(buf[1], 3.0f));
        }

        // A descriptor carrying MORE params than the plugin has ports (a project
        // saved against a later build) must not corrupt anything.
        EffectDesc over = d;
        over.params = { 2.0f, 9.0f, 9.0f, 9.0f };
        std::unique_ptr<IEffect> fx2 = MakeEffect(over, 48000.0);
        CHECK(fx2 != nullptr);
        if (fx2) {
            fx2->Prepare(48000.0);
            std::vector<float> buf(2, 1.0f);
            fx2->Process(buf.data(), 1);
            CHECK(Near(buf[0], 2.0f));
        }
    }

    // --- ControlValues: what the engine publishes to an open editor --------
    //
    // Automation drives SetParam from the audio thread and never writes the
    // model, so for a plugin insert the control buffer the engine holds IS the
    // parameter state. An editor showing automation therefore depends on this
    // reporting exactly what was set -- a stale or clamped-away value here is
    // an editor that lies about what is playing.
    {
        std::unique_ptr<IEffect> fx = host.Create(kMonoGain, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            float vals[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
            CHECK(fx->ControlValues(vals, 4) == 1);        // exactly one parameter
            CHECK(Near(vals[0], 1.0f));                    // its declared default

            fx->SetParam(0, 2.5f);
            CHECK(fx->ControlValues(vals, 4) == 1);
            CHECK(Near(vals[0], 2.5f));

            // The port's declared maximum, because that is what the plugin will
            // actually read on its next run() -- ControlValues must not report
            // the value the caller asked for, only the one that took effect.
            fx->SetParam(0, 99.0f);
            CHECK(fx->ControlValues(vals, 4) == 1);
            CHECK(Near(vals[0], 4.0f));

            // A slot the plugin does not have is not a parameter.
            fx->SetParam(7, 1.0f);
            CHECK(fx->ControlValues(vals, 4) == 1);
            CHECK(Near(vals[0], 4.0f));

            // Fewer slots asked for than exist: writes what fits and says so.
            vals[0] = -1.0f;
            CHECK(fx->ControlValues(vals, 0) == 0);
            CHECK(Near(vals[0], -1.0f));                   // buffer untouched
        }

        // A built-in effect has no control ports at all. It must report zero --
        // the engine publishes nothing for it, rather than publishing whatever
        // happened to be in an uninitialised buffer.
        EffectDesc eq;
        eq.type = EffectType::Eq;
        eq.params = { 1000.0f, 0.0f, 1.0f };
        std::unique_ptr<IEffect> builtin = MakeEffect(eq, 48000.0);
        CHECK(builtin != nullptr);
        if (builtin) {
            float vals[2] = { -1.0f, -1.0f };
            CHECK(builtin->ControlValues(vals, 2) == 0);
            CHECK(Near(vals[0], -1.0f));
        }
    }

    // --- UiRequiresInstanceAccess: which editors may be linked ------------
    //
    // This decides whether the host drives a plugin's own GUI through control
    // ports or only mirrors values into it. Getting it wrong is silent in both
    // directions: a direct-access UI linked through the write function looks
    // live and is not, and one wrongly called direct-access looks view-only
    // forever. The fixture bundle declares one of each, so both answers are
    // pinned rather than assumed.
    {
        CHECK(host.UiRequiresInstanceAccess(kMonoGain) == true);
        CHECK(host.UiRequiresInstanceAccess(kStereoLatent) == false);
        // Not a plugin at all, and a plugin with no UI: conservative, because
        // the safe mistake is leaving an editor unlinked.
        CHECK(host.UiRequiresInstanceAccess("urn:haiku-daw:test:does-not-exist")
              == true);
        CHECK(host.UiRequiresInstanceAccess(kBadTopology) == true);
    }

    std::printf("lv2_fixture_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
