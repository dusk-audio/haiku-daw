// Host-buildable tests for FX insert slots: the per-insert bypass / wet-dry
// fields on EffectDesc, their optional `fxin` / `masterfxin` serialization, the
// EffectType::Lv2 factory hook, and the commands that drive them.
//
// The format rule under test: bypass/mix live on OPTIONAL trailing lines that
// are written only for a non-default insert, so a project that never touches an
// insert is byte-identical to a pre-feature save, and a file that does carry
// them still loads in a build that has never heard of the keyword.

#include "../src/dsp/EffectFactory.h"
#include "../src/dsp/IEffect.h"
#include "../src/model/Commands.h"
#include "../src/model/ProjectIO.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kPath = "fx_insert_test_tmp.dawproj";

static std::string ReadFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void WriteFile(const std::string& p, const std::string& text) {
    std::ofstream f(p);
    f << text;
}
static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Save a project holding one track whose chain is `fx` (and whose MASTER chain
// is `masterFx`), then reload it. Returns false if the reload failed.
static bool SaveLoad(const std::vector<EffectDesc>& fx,
                     const std::vector<EffectDesc>& masterFx, Project& out) {
    Project a;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), a);
    a.Tracks().front().fx = fx;
    a.masterFx = masterFx;
    if (!ProjectIO::Save(a, kPath)) return false;
    return ProjectIO::Load(out, kPath);
}

// Save as above, then replace the whole first line whose leading keyword is
// `keyword` with `to` (which must carry its own newline, or be "" to delete the
// line), and load the result. Mirrors instrument_io_tests::LoadPatched —
// patching a real save beats hand-writing a format with a dozen fields on the
// track line alone — but matches by KEYWORD rather than by exact text, since
// the writer emits floats at full precision ("0.400000006", not "0.4").
static bool LoadPatched(const std::vector<EffectDesc>& fx,
                        const std::string& keyword, const std::string& to,
                        Project& out) {
    Project a;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), a);
    a.Tracks().front().fx = fx;
    if (!ProjectIO::Save(a, kPath)) return false;

    std::string       text = ReadFile(kPath);
    std::stringstream in(text), outText;
    std::string       line;
    bool              patched = false;
    while (std::getline(in, line)) {
        std::string kw;
        std::istringstream(line) >> kw;
        if (!patched && kw == keyword) {
            outText << to;          // "" deletes the line outright
            patched = true;
            continue;
        }
        outText << line << "\n";
    }
    if (!patched) return false;     // the line we meant to patch wasn't there
    WriteFile(kPath, outText.str());
    return ProjectIO::Load(out, kPath);
}

// One-effect chain whose insert state is NON-default, so the save actually
// emits an `fxin` line for LoadPatched to rewrite.
static std::vector<EffectDesc> OneMarked() {
    std::vector<EffectDesc> fx = { EqDesc() };
    fx[0].bypassed = true;
    fx[0].mix      = 0.4f;
    return fx;
}

// A stand-in for a real LV2 instance, so the factory hook can be exercised
// without lilv (or Haiku).
class StubLv2 : public IEffect {
public:
    void Prepare(double sr) override { fRate = sr; }
    void Process(float*, int) override {}
    void Reset() override {}
    void SetParam(int slot, float v) override {
        if (slot >= 0 && slot < 8) fParams[slot] = v;
    }
    const char* Name() const override { return "StubLv2"; }
    double fRate = 0.0;
    float  fParams[8] = {0};
};
// Recorded by the hook so the test can assert what MakeEffect forwarded.
static std::string gLastUri;
static double      gLastRate = 0.0;
static IEffect* StubLv2Factory(const EffectDesc& d, double sampleRate) {
    gLastUri  = d.pluginName;
    gLastRate = sampleRate;
    if (d.pluginName == "urn:missing") return nullptr;   // unresolvable URI
    return new StubLv2();
}

int main() {
    // --- The pinned type contract ---------------------------------------
    // Other agents (the LV2 host, the slot UI) and every saved file depend on
    // these exact ids, so pin them here rather than trusting the enum order.
    CHECK((int)EffectType::Lv2 == 10);
    CHECK((int)EffectType::Limiter == 9);   // Lv2 was APPENDED, nothing moved
    CHECK(kMaxEffectTypeId == 10);

    // Defaults: an untouched insert is fully wet and not bypassed, i.e. the
    // pre-feature behavior.
    {
        EffectDesc d;
        CHECK(d.bypassed == false);
        CHECK(d.mix == 1.0f);
        // The new fields are declared last, so every existing brace-init of the
        // leading fields still compiles and still gets the defaults.
        EffectDesc braced{EffectType::Delay, {0.25f, 0.35f, 0.3f}};
        CHECK(braced.bypassed == false);
        CHECK(braced.mix == 1.0f);
        CHECK(EqDesc().mix == 1.0f);
    }

    // --- Round-trip: track chain ----------------------------------------
    {
        std::vector<EffectDesc> fx = { EqDesc(), SaturatorDesc(), DelayDesc() };
        fx[0].bypassed = true;               // bypassed, still fully wet
        fx[2].mix      = 0.25f;              // wet/dry, not bypassed
        Project b;
        CHECK(SaveLoad(fx, {}, b));
        CHECK(!b.Tracks().empty());
        const std::vector<EffectDesc>& got = b.Tracks().front().fx;
        CHECK(got.size() == 3);
        if (got.size() == 3) {
            CHECK(got[0].bypassed == true);
            CHECK(got[0].mix == 1.0f);
            CHECK(got[1].bypassed == false);   // untouched slot keeps defaults
            CHECK(got[1].mix == 1.0f);
            CHECK(got[2].bypassed == false);
            CHECK(got[2].mix == 0.25f);
        }
        // Only the two non-default inserts got a line; slot 1 did not.
        const std::string text = ReadFile(kPath);
        CHECK(Contains(text, "fxin 0 1 1"));
        CHECK(Contains(text, "fxin 2 0 0.25"));
        CHECK(!Contains(text, "fxin 1 "));
    }

    // A chain that is entirely default writes no fxin line at all, so a project
    // that never touches an insert stays byte-identical to a pre-feature save.
    {
        Project b;
        CHECK(SaveLoad({ EqDesc(), SaturatorDesc() }, {}, b));
        CHECK(!Contains(ReadFile(kPath), "fxin"));
    }

    // --- Round-trip: master chain ---------------------------------------
    {
        std::vector<EffectDesc> mfx = { CompressorDesc(), LimiterDesc() };
        mfx[0].mix      = 0.5f;
        mfx[1].bypassed = true;
        Project b;
        CHECK(SaveLoad({}, mfx, b));
        CHECK(b.masterFx.size() == 2);
        if (b.masterFx.size() == 2) {
            CHECK(b.masterFx[0].mix == 0.5f);
            CHECK(b.masterFx[0].bypassed == false);
            CHECK(b.masterFx[1].bypassed == true);
            CHECK(b.masterFx[1].mix == 1.0f);
        }
        const std::string text = ReadFile(kPath);
        CHECK(Contains(text, "masterfxin 0 0 0.5"));
        CHECK(Contains(text, "masterfxin 1 1 1"));
    }

    // --- Compat direction 1: a NEW build reads an OLD file ---------------
    // An old file has no fxin line. Strip it and the insert must fall back to
    // the EffectDesc defaults, i.e. behave exactly as it did before the feature.
    {
        std::vector<EffectDesc> fx = { EqDesc() };
        fx[0].bypassed = true;
        fx[0].mix      = 0.4f;
        Project b;
        CHECK(LoadPatched(fx, "fxin", "", b));
        CHECK(!b.Tracks().empty());
        CHECK(b.Tracks().front().fx.size() == 1);
        if (b.Tracks().front().fx.size() == 1) {
            CHECK(b.Tracks().front().fx[0].bypassed == false);
            CHECK(b.Tracks().front().fx[0].mix == 1.0f);
        }
    }

    // --- Compat direction 2: an OLD build reads a NEW file ---------------
    // There is no old binary to run here, so assert the property this repo
    // actually controls and that the claim rests on: the loader's final else
    // chain IGNORES a keyword it doesn't recognize, so an unknown line neither
    // fails the load nor disturbs the records around it. `fxin` is exactly such
    // a line to a pre-feature build.
    {
        std::vector<EffectDesc> fx = { EqDesc(), SaturatorDesc() };
        fx[1].mix = 0.5f;
        Project b;
        CHECK(LoadPatched(fx, "fxin",
                          "fxin 1 0 0.5\nsomekeywordfromthefuture 1 2 three\n", b));
        CHECK(!b.Tracks().empty());
        CHECK(b.Tracks().front().fx.size() == 2);
        if (b.Tracks().front().fx.size() == 2)
            CHECK(b.Tracks().front().fx[1].mix == 0.5f);
    }

    // The closest thing to actually RUNNING an old binary: take a file that
    // carries `fxin` and rename the keyword to one THIS build does not know.
    // The loader then treats the line exactly as a pre-feature build treats
    // `fxin` — it falls through to the ignore-unknown branch — so what comes out
    // is precisely what an old build would have produced. Everything else on the
    // track must survive intact, and the insert state must fall back to the
    // EffectDesc defaults (which is also the "lossy downgrade" documented in the
    // PR: the data is skipped, not preserved).
    {
        std::vector<EffectDesc> fx = { EqDesc(), SaturatorDesc() };
        fx[1].bypassed = true;
        fx[1].mix      = 0.5f;
        Project b;
        CHECK(LoadPatched(fx, "fxin", "fxinFROMNEWERBUILD 1 1 0.5\n", b));
        CHECK(!b.Tracks().empty());
        if (!b.Tracks().empty()) {
            const Track& t = b.Tracks().front();
            CHECK(t.fx.size() == 2);          // the chain itself is untouched
            if (t.fx.size() == 2) {
                CHECK(t.fx[0].type == EffectType::Eq);
                CHECK(t.fx[1].type == EffectType::Saturator);
                CHECK(t.fx[1].params.size() == 3);   // params still parsed
                CHECK(t.fx[1].bypassed == false);    // ...insert state dropped
                CHECK(t.fx[1].mix == 1.0f);
            }
            CHECK(t.name == "T");             // records after the skipped line
        }
    }

    // --- Malformed fxin: clamp the mix, skip a bad index ------------------
    // Two deliberately different policies. A mix outside [0,1] is meaningless
    // but harmless, so it clamps; an index that cannot address a slot has
    // nothing to attach to, so the record is skipped — and neither fails the
    // load, unlike an out-of-range `mev` field.
    {
        Project b;
        CHECK(LoadPatched(OneMarked(), "fxin", "fxin 0 0 5.0\n", b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
        if (!b.Tracks().empty() && b.Tracks().front().fx.size() == 1)
            CHECK(b.Tracks().front().fx[0].mix == 1.0f);
    }
    {
        Project b;
        CHECK(LoadPatched(OneMarked(), "fxin", "fxin 0 0 -2.5\n", b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
        if (!b.Tracks().empty() && b.Tracks().front().fx.size() == 1)
            CHECK(b.Tracks().front().fx[0].mix == 0.0f);
    }
    {
        Project b;   // index past the end: ignored, load still succeeds
        CHECK(LoadPatched(OneMarked(), "fxin", "fxin 7 1 0.2\n", b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
        if (!b.Tracks().empty() && b.Tracks().front().fx.size() == 1) {
            CHECK(b.Tracks().front().fx[0].bypassed == false);
            CHECK(b.Tracks().front().fx[0].mix == 1.0f);
        }
    }
    {
        Project b;   // negative index: likewise ignored, not fatal
        CHECK(LoadPatched(OneMarked(), "fxin", "fxin -1 1 0.2\n", b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
    }
    {
        Project b;   // truncated line: the unread fields keep their defaults
        CHECK(LoadPatched(OneMarked(), "fxin", "fxin 0 1\n", b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
        if (!b.Tracks().empty() && b.Tracks().front().fx.size() == 1) {
            CHECK(b.Tracks().front().fx[0].bypassed == true);
            CHECK(b.Tracks().front().fx[0].mix == 1.0f);
        }
    }

    // An Lv2 descriptor survives a round trip: the type id and the URI it
    // carries in pluginName, plus its insert state.
    {
        EffectDesc lv2;
        lv2.type       = EffectType::Lv2;
        lv2.pluginName = "http://example.org/plugin";
        lv2.params     = { 0.5f, 0.25f };
        lv2.mix        = 0.75f;
        Project b;
        CHECK(SaveLoad({ lv2 }, { lv2 }, b));
        CHECK(!b.Tracks().empty() && b.Tracks().front().fx.size() == 1);
        if (!b.Tracks().empty() && b.Tracks().front().fx.size() == 1) {
            const EffectDesc& g = b.Tracks().front().fx[0];
            CHECK(g.type == EffectType::Lv2);
            CHECK(g.params.size() == 2);
            CHECK(g.mix == 0.75f);
            // The URI is the whole identity of an LV2 insert: drop it and the
            // plugin can never be re-instantiated, so the chain silently loses
            // an effect. Serialization gates on EffectHasPluginName, NOT on
            // `type == Plugin` — this assertion is what pins that down.
            CHECK(g.pluginName == "http://example.org/plugin");
        }
        // ...and the same on the master chain, which has its own writer/reader.
        CHECK(b.masterFx.size() == 1);
        if (b.masterFx.size() == 1) {
            CHECK(b.masterFx[0].type == EffectType::Lv2);
            CHECK(b.masterFx[0].pluginName == "http://example.org/plugin");
            CHECK(b.masterFx[0].mix == 0.75f);
        }
        // A native Plugin's add-on id must still round-trip unchanged.
        EffectDesc plug;
        plug.type       = EffectType::Plugin;
        plug.pluginName = "SomeAddOn";
        Project c;
        CHECK(SaveLoad({ plug }, {}, c));
        CHECK(!c.Tracks().empty() && c.Tracks().front().fx.size() == 1);
        if (!c.Tracks().empty() && c.Tracks().front().fx.size() == 1)
            CHECK(c.Tracks().front().fx[0].pluginName == "SomeAddOn");
    }

    // --- The Lv2 factory hook -------------------------------------------
    {
        EffectDesc lv2;
        lv2.type       = EffectType::Lv2;
        lv2.pluginName = "urn:test";
        lv2.params     = { 0.25f, 0.5f };

        // No hook installed (every non-Haiku host): same fallback as a native
        // plugin whose add-on isn't available — nullptr, so the caller keeps an
        // index-aligned hole rather than substituting some other effect.
        EffectDesc plug;
        plug.type       = EffectType::Plugin;
        plug.pluginName = "no-such-addon";
        CHECK(MakeEffect(plug) == nullptr);
        CHECK(MakeEffect(lv2) == nullptr);
        CHECK(MakeEffect(lv2, 48000.0) == nullptr);

        SetLv2Factory(&StubLv2Factory);
        auto e = MakeEffect(lv2, 44100.0);
        CHECK(e != nullptr);
        CHECK(gLastUri == "urn:test");     // the whole desc reaches the hook...
        CHECK(gLastRate == 44100.0);       // ...along with the target rate
        if (e) {
            StubLv2* s = static_cast<StubLv2*>(e.get());
            CHECK(s->fParams[0] == 0.25f); // stored params applied via SetParam
            CHECK(s->fParams[1] == 0.5f);
        }

        // A hook that can't resolve the URI degrades the same way as no hook.
        EffectDesc missing = lv2;
        missing.pluginName = "urn:missing";
        CHECK(MakeEffect(missing, 44100.0) == nullptr);

        SetLv2Factory(nullptr);            // leave the global as we found it
        CHECK(MakeEffect(lv2, 44100.0) == nullptr);
    }

    // --- Commands ---------------------------------------------------------
    // SetFxCommand replaces the whole chain of descriptors, so bypass/mix ride
    // along with it for free — including on undo.
    {
        Project p;
        CommandStack stack;
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), p);
        const TrackId id = p.Tracks().front().id;

        std::vector<EffectDesc> v1 = { EqDesc() };
        stack.Execute(std::make_unique<SetFxCommand>(id, false, v1), p);

        std::vector<EffectDesc> v2 = { EqDesc() };
        v2[0].bypassed = true;
        v2[0].mix      = 0.3f;
        stack.Execute(std::make_unique<SetFxCommand>(id, false, v2), p);
        CHECK(p.FindTrack(id)->fx[0].bypassed == true);
        CHECK(p.FindTrack(id)->fx[0].mix == 0.3f);

        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[0].bypassed == false);
        CHECK(p.FindTrack(id)->fx[0].mix == 1.0f);

        // The master chain goes through the same command.
        std::vector<EffectDesc> m = { LimiterDesc() };
        m[0].mix = 0.6f;
        stack.Execute(std::make_unique<SetFxCommand>(id, true, m), p);
        CHECK(p.masterFx.size() == 1 && p.masterFx[0].mix == 0.6f);
        stack.Undo(p);
        CHECK(p.masterFx.empty());
    }

    // SetFxBypassCommand: a one-slot toggle with its own undo name.
    {
        Project p;
        CommandStack stack;
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), p);
        const TrackId id = p.Tracks().front().id;
        stack.Execute(std::make_unique<SetFxCommand>(
            id, false, std::vector<EffectDesc>{ EqDesc(), DelayDesc() }), p);

        CHECK(SetFxBypassCommand(id, 0, true).Name()  == "Bypass Effect");
        CHECK(SetFxBypassCommand(id, 0, false).Name() == "Enable Effect");

        CHECK(stack.Execute(std::make_unique<SetFxBypassCommand>(id, 1, true), p));
        CHECK(p.FindTrack(id)->fx[1].bypassed == true);
        CHECK(p.FindTrack(id)->fx[0].bypassed == false);   // only slot 1 moved
        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[1].bypassed == false);
        stack.Redo(p);
        CHECK(p.FindTrack(id)->fx[1].bypassed == true);

        // Two toggles are two undo steps: this command deliberately does NOT
        // coalesce, unlike a knob drag.
        CHECK(stack.Execute(std::make_unique<SetFxBypassCommand>(id, 0, true), p));
        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[0].bypassed == false);
        CHECK(p.FindTrack(id)->fx[1].bypassed == true);    // slot 1 still set

        // An index that addresses no slot is not an edit, so it never lands on
        // the undo stack.
        CHECK(!stack.Execute(std::make_unique<SetFxBypassCommand>(id, 9, true), p));
        CHECK(!stack.Execute(std::make_unique<SetFxBypassCommand>(id, -1, true), p));
        CHECK(!stack.Execute(
            std::make_unique<SetFxBypassCommand>((TrackId)9999, 0, true), p));
    }

    // SetFxParamCommand: the narrow, undoable parameter edit a native plugin
    // editor commits. It must touch ONLY the named slots -- SetFxCommand (the
    // whole-chain replace) would pass a test that overwrote the chain too.
    {
        Project p;
        CommandStack stack;
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), p);
        const TrackId id = p.Tracks().front().id;
        stack.Execute(std::make_unique<SetFxCommand>(
            id, false, std::vector<EffectDesc>{ EqDesc(), DelayDesc() }), p);

        std::vector<float> before = p.FindTrack(id)->fx[0].params;
        const size_t slot = 2;
        const float oldValue = before[slot];

        CHECK(stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, 0, std::vector<SetFxParamCommand::SlotValue>{
                              { (int)slot, oldValue + 1.0f } }), p));
        CHECK(p.FindTrack(id)->fx[0].params[slot] == oldValue + 1.0f);
        // Every other parameter, and every other insert, is untouched.
        bool othersSame = p.FindTrack(id)->fx[0].params.size() == before.size();
        for (size_t i = 0; othersSame && i < before.size(); i++)
            if (i != slot && p.FindTrack(id)->fx[0].params[i] != before[i])
                othersSame = false;
        CHECK(othersSame);
        CHECK(p.FindTrack(id)->fx[1].params == DelayDesc().params);

        // Undo restores exactly the old value, not the whole descriptor.
        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[0].params[slot] == oldValue);
        stack.Redo(p);
        CHECK(p.FindTrack(id)->fx[0].params[slot] == oldValue + 1.0f);

        // Several slots in one command (a plugin's own preset load).
        const float a0 = p.FindTrack(id)->fx[0].params[0];
        const float a1 = p.FindTrack(id)->fx[0].params[1];
        CHECK(stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, 0, std::vector<SetFxParamCommand::SlotValue>{
                              { 0, a0 + 2.0f }, { 1, a1 + 3.0f } }), p));
        CHECK(p.FindTrack(id)->fx[0].params[0] == a0 + 2.0f);
        CHECK(p.FindTrack(id)->fx[0].params[1] == a1 + 3.0f);
        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[0].params[0] == a0);
        CHECK(p.FindTrack(id)->fx[0].params[1] == a1);

        // The master chain is reached through the same flag SetFxLive uses.
        stack.Execute(std::make_unique<SetFxCommand>(
            id, true, std::vector<EffectDesc>{ LimiterDesc() }), p);
        const float mOld = p.masterFx[0].params[0];
        CHECK(stack.Execute(std::make_unique<SetFxParamCommand>(
            id, true, 0, std::vector<SetFxParamCommand::SlotValue>{
                              { 0, mOld + 0.5f } }), p));
        CHECK(p.masterFx[0].params[0] == mOld + 0.5f);
        CHECK(p.FindTrack(id)->fx[0].params[0] == a0);   // track chain untouched
        stack.Undo(p);
        CHECK(p.masterFx[0].params[0] == mOld);

        // Nothing in range is not an edit: no Do, so nothing on the undo stack.
        // A slot past the end is a plugin whose port count changed under a saved
        // project -- it must not take the whole command down with it either.
        CHECK(!stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, 9, std::vector<SetFxParamCommand::SlotValue>{ { 0, 1.0f } }), p));
        CHECK(!stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, -1, std::vector<SetFxParamCommand::SlotValue>{ { 0, 1.0f } }), p));
        CHECK(!stack.Execute(std::make_unique<SetFxParamCommand>(
            (TrackId)9999, false, 0,
            std::vector<SetFxParamCommand::SlotValue>{ { 0, 1.0f } }), p));
        const float keep = p.FindTrack(id)->fx[0].params[0];
        CHECK(!stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, 0,
            std::vector<SetFxParamCommand::SlotValue>{ { 9999, 1.0f } }), p));
        CHECK(p.FindTrack(id)->fx[0].params[0] == keep);

        // One out-of-range slot alongside a good one: the good one still lands,
        // and the undo restores only it.
        CHECK(stack.Execute(std::make_unique<SetFxParamCommand>(
            id, false, 0, std::vector<SetFxParamCommand::SlotValue>{
                              { 0, keep + 4.0f }, { 9999, 1.0f } }), p));
        CHECK(p.FindTrack(id)->fx[0].params[0] == keep + 4.0f);
        stack.Undo(p);
        CHECK(p.FindTrack(id)->fx[0].params[0] == keep);
    }

    std::remove(kPath);
    std::printf("fx_insert_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
