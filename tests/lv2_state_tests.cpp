// LV2 state and presets, against the repo's own fixture bundle.
//
// The fixture's `urn:haiku-daw:test:stateful` keeps a multiplier that is NOT a
// control port — only its state:interface can carry it — so these tests assert
// on AUDIO: a state that saved and restored correctly changes what the plugin
// multiplies by. That is the difference between "a string came back" and "the
// plugin's patch came back", and it is the whole point of the feature.
//
// Also here: the bundled preset (label, port value, state property) discovered
// through lilv, the user preset store through the host's own API, and the
// factory path that makes a loaded project come up on its saved patch.
//
// Needs lilv and the fixture bundle (the build points LV2_PATH at it); skipped
// as a pass when the bundle is not there, like lv2_fixture_tests.

#include "../src/plugin/Lv2Host.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/plugin/Lv2PresetStore.h"
#include "../src/model/Effect.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kStateful = "urn:haiku-daw:test:stateful";
static const char* kMonoGain = "urn:haiku-daw:test:mono-gain";

// A state document that sets the fixture's internal multiplier. Written by hand
// rather than by the host, so the test does not depend on the very code it is
// checking to produce its input.
//
// The shape is lilv's contract, not ours: the string form of a state must hold
// exactly ONE subject typed pset:Preset with an lv2:appliesTo, and its saved
// properties hang off the state:state object (the pset:, lv2:, state: and xsd:
// prefixes are predeclared by lilv's own reader). A document missing any of that
// is not "an empty state" — lilv refuses it outright, which is why LoadState can
// report failure at all.
static std::string MulState(double mul) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", mul);
    return std::string("<urn:haiku-daw:test:state:1>\n"
                       "    a pset:Preset ;\n"
                       "    lv2:appliesTo <urn:haiku-daw:test:stateful> ;\n"
                       "    state:state [\n"
                       "        <urn:haiku-daw:test:state:mul> \"") + buf +
           "\"^^xsd:float\n"
           "    ] .\n";
}

// Run one block of `in` through `fx` and return the left channel's output —
// the fixture multiplies both channels by the same factor.
static float RunOne(IEffect* fx, float in) {
    float block[8];
    for (int i = 0; i < 4; i++) { block[i * 2] = in; block[i * 2 + 1] = in; }
    fx->Process(block, 4);
    return block[0];
}

// Process a fresh instance twice: once before any state, once after LoadState.
static bool LoadedStateChangesAudio(const std::string& state, float* before,
                                    float* after) {
    std::unique_ptr<IEffect> fx = Lv2Host::Instance().Create(kStateful, 48000.0);
    if (!fx) return false;
    fx->Prepare(48000.0);
    *before = RunOne(fx.get(), 1.0f);
    if (!fx->LoadState(state)) return false;
    *after = RunOne(fx.get(), 1.0f);
    return true;
}

int main() {
    // Presets written by this test go to a scratch directory, never the user's.
    const std::string presetDir =
        std::string("/tmp/daw_lv2_state_preset_") + std::to_string(::getpid());
    ::setenv("DAW_LV2_PRESET_DIR", presetDir.c_str(), 1);

    Lv2Host& host = Lv2Host::Instance();
    host.ScanAll();

    const Lv2PluginInfo* info = host.Find(kStateful);
    if (!info) {
        // The fixture is built beside this test and the build points LV2_PATH
        // at it, so a missing plugin means the harness broke — not a machine
        // that happens to have no plugins. Pass silently and every assertion
        // below would vacuously "pass" against nothing.
        std::printf("  FAIL fixture bundle not found (LV2_PATH not set?)\n");
        std::printf("lv2_state_tests: 1 checks, 1 failures\n");
        return 1;
    }

    // ---- the plugin's shape ------------------------------------------------
    CHECK(info->paramSymbols.size() == info->params.size());
    CHECK(info->params.size() == 1);
    if (info->params.size() == 1) {
        CHECK(info->params[0].mn == 0.0f);
        CHECK(info->params[0].mx == 4.0f);
        CHECK(info->paramSymbols[0] == "gain");
    }

    // ---- state save / restore, asserted on the AUDIO -----------------------
    {
        float before = 0.0f, after = 0.0f;
        CHECK(LoadedStateChangesAudio(MulState(0.25), &before, &after));
        CHECK(before == 1.0f);              // untouched instance: mul = 1
        CHECK(std::fabs(after - 0.25f) < 1e-5f);

        // The instance's CURRENT state can be serialized again, and what comes
        // out restores into a fresh instance — the save/load round trip a
        // project file depends on.
        std::unique_ptr<IEffect> fx = host.Create(kStateful, 48000.0);
        CHECK(fx != nullptr);
        if (fx) {
            fx->Prepare(48000.0);
            CHECK(fx->LoadState(MulState(3.0)));
            CHECK(fx->SaveState(nullptr) == false);   // no out pointer

            std::string blob;
            CHECK(fx->SaveState(&blob));
            CHECK(!blob.empty());
            CHECK(blob.find("urn:haiku-daw:test:state:mul") != std::string::npos);
            CHECK(blob.find("3") != std::string::npos);   // the value is in there

            std::unique_ptr<IEffect> fx2 = host.Create(kStateful, 48000.0);
            CHECK(fx2 != nullptr);
            if (fx2) {
                fx2->Prepare(48000.0);
                CHECK(fx2->LoadState(blob));
                CHECK(std::fabs(RunOne(fx2.get(), 1.0f) - 3.0f) < 1e-5f);
            }
        }

        // A fresh instance's state does NOT contain the multiplier we set
        // elsewhere, and a state document that is not a state document is
        // refused without disturbing the instance.
        std::unique_ptr<IEffect> clean = host.Create(kStateful, 48000.0);
        CHECK(clean != nullptr);
        if (clean) {
            clean->Prepare(48000.0);
            std::string blob;
            CHECK(clean->SaveState(&blob));
            CHECK(blob.find("0.250000") == std::string::npos);
            CHECK(!clean->LoadState("this is not turtle"));
            CHECK(!clean->LoadState(""));
            CHECK(std::fabs(RunOne(clean.get(), 1.0f) - 1.0f) < 1e-5f);
        }

        // A built-in has no state at all — the default IEffect answers, and
        // MakeEffect's call path stays a no-op for it.
        EffectDesc eq = EqDesc();
        std::unique_ptr<IEffect> builtin = MakeEffect(eq, 48000.0);
        CHECK(builtin != nullptr);
        if (builtin) {
            std::string blob = "untouched";
            CHECK(!builtin->SaveState(&blob));
            CHECK(blob == "untouched");
            CHECK(!builtin->LoadState("anything"));
        }
    }

    // ---- the factory applies a descriptor's state --------------------------
    {
        // This is the load path: a project holds the blob, the engine builds
        // the effect from the descriptor, and the effect must come up already
        // patched — before anything runs it.
        std::unique_ptr<IEffect> seed = host.Create(kStateful, 48000.0);
        CHECK(seed != nullptr);
        std::string blob;
        if (seed) {
            seed->Prepare(48000.0);
            seed->LoadState(MulState(2.5));
            CHECK(seed->SaveState(&blob));
        }

        EffectDesc d;
        d.type = EffectType::Lv2;
        d.pluginName = kStateful;
        d.params = { 1.0f };
        d.state = blob;
        std::unique_ptr<IEffect> fromDesc = MakeEffect(d, 48000.0);
        CHECK(fromDesc != nullptr);
        if (fromDesc) {
            fromDesc->Prepare(48000.0);
            CHECK(std::fabs(RunOne(fromDesc.get(), 1.0f) - 2.5f) < 1e-5f);
        }

        // ...and the stored params go on top of it, not the other way round.
        d.params = { 2.0f };
        std::unique_ptr<IEffect> withParams = MakeEffect(d, 48000.0);
        CHECK(withParams != nullptr);
        if (withParams) {
            withParams->Prepare(48000.0);
            CHECK(std::fabs(RunOne(withParams.get(), 1.0f) - 5.0f) < 1e-5f);
        }
    }

    // ---- presets -----------------------------------------------------------
    {
        std::vector<Lv2PresetInfo> presets = host.Presets(kStateful);
        bool sawHalf = false;
        for (const Lv2PresetInfo& p : presets) {
            if (p.name != "Half Gain") continue;
            sawHalf = true;
            CHECK(!p.state.empty());
            CHECK(p.params.size() == 1);
            if (p.params.size() == 1) {
                CHECK(p.params[0].first == 0);          // the "gain" slot
                CHECK(std::fabs(p.params[0].second - 0.5f) < 1e-5f);
            }
            // The preset's state property reaches the plugin too: loading this
            // preset must set the internal multiplier, not only the port.
            std::unique_ptr<IEffect> fx = host.Create(kStateful, 48000.0);
            CHECK(fx != nullptr);
            if (fx) {
                fx->Prepare(48000.0);
                fx->SetParam(0, 0.5f);                  // what the UI would do
                CHECK(fx->LoadState(p.state));
                CHECK(std::fabs(RunOne(fx.get(), 1.0f) - 2.0f) < 1e-4f);  // 0.5 * 4
            }
        }
        CHECK(sawHalf);

        // Another plugin sees none of them.
        for (const Lv2PresetInfo& p : host.Presets(kMonoGain))
            CHECK(p.name != "Half Gain");

        // A preset keeps values the port's own metadata would not accept.
        const std::vector<std::pair<int, float>> clamped =
            host.PresetParams(kStateful, MulState(1.0));
        CHECK(clamped.empty());   // state-only property, no port values
    }

    // ---- the user store, through the host's API ----------------------------
    {
        const std::string name = "My Patch";
        CHECK(host.SavePreset(kStateful, name, "state body", { 3.0f }));
        CHECK(!host.SavePreset("urn:no:such:plugin", name, "x", {}));
        CHECK(!host.SavePreset(kStateful, "", "x", {}));

        bool sawMine = false;
        for (const Lv2PresetInfo& p : host.Presets(kStateful)) {
            if (p.name != name) continue;
            sawMine = true;
            CHECK(p.state == "state body");
            CHECK(p.params.size() == 1);
            if (p.params.size() == 1) {
                CHECK(p.params[0].first == 0);
                CHECK(p.params[0].second == 3.0f);
            }
        }
        CHECK(sawMine);

        // ...and it is not offered for another plugin.
        for (const Lv2PresetInfo& p : host.Presets(kMonoGain))
            CHECK(p.name != name);
    }

    // ---- PresetParams: symbol -> slot, and the port's domain --------------
    {
        // A state carrying a state-only property and no port values: nothing
        // maps to a slot, and asking is not an error.
        CHECK(host.PresetParams(kStateful, MulState(1.0)).empty());

        // Port values DO map, by SYMBOL — a symbol the plugin does not have is
        // dropped, and a value the port's own range forbids is clamped, which is
        // the same coercion SetParam applies.
        const std::string byPort =
            "<urn:haiku-daw:test:state:2>\n"
            "    a pset:Preset ;\n"
            "    lv2:appliesTo <urn:haiku-daw:test:stateful> ;\n"
            "    lv2:port [ lv2:symbol \"nope\" ; pset:value 1.0 ] ,\n"
            "             [ lv2:symbol \"gain\" ; pset:value 99.0 ] .\n";
        const std::vector<std::pair<int, float>> got =
            host.PresetParams(kStateful, byPort);
        CHECK(got.size() == 1);          // the unknown symbol is dropped
        if (got.size() == 1) {
            CHECK(got[0].first == 0);
            CHECK(got[0].second == 4.0f);   // clamped to the port's maximum
        }

        // A string that is not a state at all is refused rather than half-read.
        CHECK(host.PresetParams(kStateful, "not a state document").empty());
    }

    // cleanup
    ::unlink((presetDir + "/My_Patch.dawpreset").c_str());
    ::rmdir(presetDir.c_str());

    std::printf("lv2_state_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
