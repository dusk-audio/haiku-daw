// Engine-side tests for the live LV2 editor: the publishing half of
// engine -> editor, on the machine the engine actually builds on.
//
// Everything else about the live link is host-tested (Lv2UiMap.h,
// FxWatchTable.h, Lv2Host::UiRequiresInstanceAccess); this file covers what
// cannot be, because Engine.cpp links the Media Kit: that a watched insert
// publishes its real control values, that the values are the ones the plugin
// will actually read (clamped, per slot), that an unchanged insert does not
// publish at all, and that a watch is addressed so a rebuild cannot make it
// dangle.
//
// Built only on Haiku with LV2 enabled. Needs an audio output device for
// Engine::Load (it opens one); where there is none it says so and passes,
// like lv2_host_tests does on a machine with no plugins.

#include "../src/plugin/Lv2Host.h"
#include "../src/model/Commands.h"
#include "../src/model/Project.h"
#include "../src/dsp/EffectFactory.h"
#include "../src/engine/Engine.h"

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

// A descriptor seeded with the plugin's own port defaults. Zero-filling instead
// would switch off the `Enabled` port real plugins expose and render silence --
// the trap package 03 paid for -- and it would also make the "publishes the
// defaults" assertion below meaningless.
bool MakeLv2Desc(const Lv2PluginInfo& info, EffectDesc& out) {
    out.type = EffectType::Lv2;
    out.pluginName = info.uri;
    out.params.clear();
    for (const Lv2ParamInfo& p : info.params) out.params.push_back(p.def);
    return !info.params.empty();
}

} // namespace

int main() {
    Lv2Host& host = Lv2Host::Instance();
    host.ScanAll();

    // Any installed plugin with parameters will do: the test asserts on the
    // values it set, not on a particular plugin.
    const Lv2PluginInfo* pick = nullptr;
    for (const Lv2PluginInfo& p : host.Plugins()) {
        if (p.params.empty()) continue;
        if (!MakeEffect(EffectDesc{}, 48000.0)) { /* factory probe, ignored */ }
        pick = &p;
        break;
    }
    if (!pick) {
        std::printf("lv2_live_editor_tests: SKIP -- no LV2 plugin with "
                    "parameters is installed\n");
        return 0;
    }
    std::printf("lv2_live_editor_tests: using %s (%zu parameters)\n",
                pick->name.c_str(), pick->params.size());

    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), p);
    const TrackId tid = p.Tracks().front().id;

    // One insert, and one note so the track actually gets a bus -- the engine
    // builds its buses from content, and a bus is what carries the FX chain.
    {
        Track& t = const_cast<Track&>(p.Tracks().front());
        EffectDesc d;
        if (!MakeLv2Desc(*pick, d)) return 0;
        t.fx.push_back(d);
        MidiClip mc;
        mc.id = 1;
        mc.startFrame = 0;
        mc.lengthFrames = 96000;
        MidiNote n;
        n.pitch = 60;
        n.startFrame = 0;
        n.lengthFrames = 4800;
        mc.notes.push_back(n);
        t.midiClips.push_back(mc);
    }

    Engine e;
    e.SetBufferFrames(256);
    if (e.Load(p, 0, 96000) != B_OK) {
        std::printf("lv2_live_editor_tests: SKIP -- no audio output device "
                    "(Engine::Load failed)\n");
        return 0;
    }

    const int nParams = (int)pick->params.size();
    float vals[Engine::kWatchMax];

    // --- nothing is published without a watch -------------------------------
    {
        uint32_t gen = 123;
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, &gen) == 0);
        CHECK(gen == 123);            // untouched, not merely "no values"
        CHECK(e.WatchedFxParams(-1, vals, Engine::kWatchMax, nullptr) == 0);
        CHECK(e.WatchedFxParams(Engine::kWatchSlots, vals, Engine::kWatchMax,
                                nullptr) == 0);
    }

    // --- a watched insert publishes its real values -------------------------
    {
        e.SetFxWatch(0, tid, false, 0);
        e.PublishFxWatchNow();
        uint32_t gen0 = 0;
        const int n = e.WatchedFxParams(0, vals, Engine::kWatchMax, &gen0);
        CHECK(n == nParams);
        bool defaults = n > 0;
        for (int i = 0; i < n; i++)
            if (vals[i] != pick->params[(size_t)i].def) defaults = false;
        CHECK(defaults);              // what the insert was built with
        CHECK((gen0 & 1u) == 0u);     // an even generation: no torn frame

        // An insert nobody touched republishes nothing: the generation is the
        // UI's cheap "has anything changed?" test, so it must not tick.
        uint32_t gen1 = 0;
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, &gen1) == n);
        CHECK(gen1 == gen0);

        // A live parameter write does publish, and publishes exactly what the
        // plugin will read.
        const float want = pick->params[0].def + 0.1f;
        e.SetFxParamLive(tid, false, 0, 0, want);
        e.PublishFxWatchNow();
        uint32_t gen2 = 0;
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, &gen2) == n);
        CHECK(gen2 != gen0);
        const float got = vals[0];
        CHECK(got >= pick->params[0].mn && got <= pick->params[0].mx);
        if (pick->params[0].mx > pick->params[0].mn)
            CHECK(got == want || got == pick->params[0].mx
                            || got == pick->params[0].mn);

        // Out of range is clamped HERE too, because the value that matters is
        // the one the plugin reads, not the one the caller asked for.
        e.SetFxParamLive(tid, false, 0, 0, pick->params[0].mx + 1000.0f);
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, nullptr) == n);
        CHECK(vals[0] == pick->params[0].mx);

        // A slot the insert does not have cannot corrupt a neighbour.
        e.SetFxParamLive(tid, false, 0, nParams + 5, 0.5f);
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, nullptr) == n);
        CHECK(vals[0] == pick->params[0].mx);
    }

    // --- several watches, and stopping one ----------------------------------
    {
        // A second slot watching a track that does not exist: publishes
        // nothing, and must not disturb the first.
        e.SetFxWatch(1, (TrackId)9999, false, 0);
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(1, vals, Engine::kWatchMax, nullptr) == 0);
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, nullptr) == nParams);

        // A watch on a chain that has no such insert: same.
        e.SetFxWatch(2, tid, false, 7);
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(2, vals, Engine::kWatchMax, nullptr) == 0);

        // Stopping a watch stops publishing for that slot only.
        e.SetFxWatch(1, kInvalidTrackId, false, -1);
        e.PublishFxWatchNow();
        CHECK(e.WatchedFxParams(1, vals, Engine::kWatchMax, nullptr) == 0);
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, nullptr) == nParams);

        // Re-pointing a watch at another insert must publish even if the values
        // happen to be identical -- the editor has never seen a frame.
        e.SetFxParamLive(tid, false, 0, 0, pick->params[0].mn);
        e.PublishFxWatchNow();
        uint32_t a = 0;
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, &a) == nParams);
        e.SetFxWatch(0, tid, false, 0);      // same insert, re-registered
        e.PublishFxWatchNow();
        uint32_t b = 0;
        CHECK(e.WatchedFxParams(0, vals, Engine::kWatchMax, &b) == nParams);
        CHECK(b != a);
    }

    std::printf("lv2_live_editor_tests: %d checks, %d failures\n",
                g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
