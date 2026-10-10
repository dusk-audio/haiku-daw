// Host-buildable tests for plugin state: the base64 codec the project format
// carries it in, and the `fxstate` / `masterfxstate` lines themselves.
//
// The rule under test: an insert's own state (what IEffect::SaveState gives)
// survives a save/load unchanged, and every part of the format is APPEND-ONLY —
// a project with no state line loads exactly as it did before this feature, and
// the `fx` line itself did not grow a field. Kit-free throughout: no lilv here,
// so this runs everywhere the model layer does.

#include "../src/model/Base64.h"
#include "../src/model/Commands.h"
#include "../src/model/ProjectIO.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kPath = "fx_state_io_test_tmp.dawproj";

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

// A state blob with everything the project line must not be able to mangle: a
// multi-line TTL document, quotes, backslashes and a NUL.
static std::string UglyState() {
    std::string s = "@prefix p: <urn:haiku-daw:test:prop:> .\n"
                    "<urn:haiku-daw:state:1> p:name \"A \\\"quoted\\\" patch\" ;\n"
                    "    p:body \"line one\\nline two\" .\n";
    s.push_back('\0');
    s += "tail";
    return s;
}

// Save `fx` (and `masterFx`) on one track, reload, return whether it worked.
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

// Save as above, then replace the FIRST line whose leading keyword is `keyword`
// with `to` (carrying its own newline, or "" to delete the line) — or append `to`
// when no such line exists. Mirrors instrument_io_tests::LoadPatched: patch a
// real save rather than hand-writing the format, so a change to the writer
// cannot silently pass these tests.
static bool LoadPatched(const std::vector<EffectDesc>& fx,
                        const std::string& keyword, const std::string& to,
                        Project& out) {
    Project a;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "T"), a);
    a.Tracks().front().fx = fx;
    if (!ProjectIO::Save(a, kPath)) return false;

    std::string       text = ReadFile(kPath);
    std::stringstream in(text), patched;
    std::string       line;
    bool              done = false;
    while (std::getline(in, line)) {
        std::string kw;
        std::istringstream(line) >> kw;
        if (kw == keyword && !done) { patched << to; done = true; }
        else                        patched << line << "\n";
    }
    if (!done && !to.empty()) patched << to;   // nothing to replace: append
    WriteFile(kPath, patched.str());
    return ProjectIO::Load(out, kPath);
}

static EffectDesc Lv2Insert(const std::string& uri) {
    EffectDesc d;
    d.type = EffectType::Lv2;
    d.pluginName = uri;
    d.params = { 0.25f, 1.0f };
    return d;
}

int main() {
    // ---- the codec ---------------------------------------------------------
    {
        CHECK(Base64Encode("") == "");
        CHECK(Base64Encode("f") == "Zg==");
        CHECK(Base64Encode("fo") == "Zm8=");
        CHECK(Base64Encode("foo") == "Zm9v");
        CHECK(Base64Encode("foob") == "Zm9vYg==");
        CHECK(Base64Encode("fooba") == "Zm9vYmE=");
        CHECK(Base64Encode("foobar") == "Zm9vYmFy");

        std::string out;
        CHECK(Base64Decode("", &out) && out.empty());
        CHECK(Base64Decode("Zg==", &out) && out == "f");
        CHECK(Base64Decode("Zm8=", &out) && out == "fo");
        CHECK(Base64Decode("Zm9v", &out) && out == "foo");
        CHECK(Base64Decode("Zm9vYmFy", &out) && out == "foobar");

        // Every byte value survives, which is what matters for a blob whose
        // content we do not interpret (a plugin may put anything in it).
        std::string all;
        for (int i = 0; i < 256; i++) all.push_back((char)i);
        const std::string enc = Base64Encode(all);
        CHECK(Base64Decode(enc, &out) && out == all);
        CHECK(enc.find('\n') == std::string::npos);
        CHECK(enc.find(' ') == std::string::npos);

        // Malformed input is refused, and refused WITHOUT touching the caller's
        // string: a half-decoded blob handed to a plugin as its state is worse
        // than no state at all.
        out = "untouched";
        CHECK(!Base64Decode("Zm9", &out));            // length not a multiple of 4
        CHECK(!Base64Decode("Zm9v=", &out));          // padding, then more input
        CHECK(!Base64Decode("=Zm9", &out));           // padding up front
        CHECK(!Base64Decode("Zm$9", &out));           // not the alphabet
        CHECK(!Base64Decode("Zg==Zg==", &out));       // padding in a middle group
        CHECK(!Base64Decode("Zg=Z", &out));           // a character after '='
        CHECK(!Base64Decode("Z===", &out));           // more padding than data
        CHECK(out == "untouched");
    }

    // ---- a track insert's state survives the round trip --------------------
    {
        const std::string state = UglyState();
        std::vector<EffectDesc> fx = { Lv2Insert("urn:plug:one"), DelayDesc() };
        fx[0].state = state;

        Project b;
        CHECK(SaveLoad(fx, {}, b));
        CHECK(b.Tracks().size() == 1);
        if (b.Tracks().size() == 1) {
            const std::vector<EffectDesc>& got = b.Tracks().front().fx;
            CHECK(got.size() == 2);
            if (got.size() == 2) {
                CHECK(got[0].state == state);
                CHECK(got[0].params == fx[0].params);
                CHECK(got[0].pluginName == "urn:plug:one");
                CHECK(got[1].state.empty());   // a built-in has none
            }
        }

        // The file itself: exactly one state line, naming the right index, and
        // the `fx` line unchanged from the format that existed before this
        // feature ("fx <type> <count> <params...> ["<name>"]").
        const std::string text = ReadFile(kPath);
        CHECK(Contains(text, "\nfxstate 0 "));
        CHECK(!Contains(text, "fxstate 1 "));
        CHECK(!Contains(text, "masterfxstate"));
        std::istringstream lines(text);
        std::string l;
        int fxLines = 0;
        while (std::getline(lines, l)) {
            if (l.rfind("fx ", 0) != 0) continue;
            // The FIRST fx line is the Lv2 insert, and it must read exactly as
            // it did before this feature: type, count, params, quoted URI. The
            // state is on its own line, and nothing else moved.
            if (fxLines == 0)
                CHECK(l == "fx 10 2 0.25 1 \"urn:plug:one\"");
            fxLines++;
        }
        CHECK(fxLines == 2);

        // Re-saving what was loaded is byte-identical: nothing about the state
        // is normalized, lost or double-encoded on the way through the model.
        Project c;
        CHECK(ProjectIO::Save(b, kPath));
        const std::string text2 = ReadFile(kPath);
        CHECK(text2 == text);
    }

    // ---- two inserts keep their OWN state ----------------------------------
    {
        std::vector<EffectDesc> fx = { Lv2Insert("urn:plug:a"), Lv2Insert("urn:plug:b") };
        fx[0].state = "state for the first";
        fx[1].state = "state for the second";
        Project b;
        CHECK(SaveLoad(fx, {}, b));
        if (b.Tracks().size() == 1 && b.Tracks().front().fx.size() == 2) {
            CHECK(b.Tracks().front().fx[0].state == "state for the first");
            CHECK(b.Tracks().front().fx[1].state == "state for the second");
        }
    }

    // ---- the master chain carries state too --------------------------------
    {
        std::vector<EffectDesc> master = { Lv2Insert("urn:plug:master") };
        master[0].state = "master patch";
        Project b;
        CHECK(SaveLoad({}, master, b));
        CHECK(b.masterFx.size() == 1);
        if (b.masterFx.size() == 1) CHECK(b.masterFx[0].state == "master patch");
        CHECK(Contains(ReadFile(kPath), "\nmasterfxstate 0 "));
    }

    // ---- compat, both directions ------------------------------------------
    {
        // A file written BEFORE this feature: no state line at all. The insert
        // loads with an empty state (the plugin comes up on its own defaults,
        // which is the pre-feature behaviour) and its other fields intact.
        const std::string state = UglyState();
        std::vector<EffectDesc> fx = { Lv2Insert("urn:plug:one") };
        fx[0].state = state;
        Project b;
        CHECK(LoadPatched(fx, "fxstate", "", b));
        CHECK(b.Tracks().size() == 1);
        if (b.Tracks().size() == 1 && b.Tracks().front().fx.size() == 1) {
            CHECK(b.Tracks().front().fx[0].state.empty());
            CHECK(b.Tracks().front().fx[0].params == fx[0].params);
            CHECK(b.Tracks().front().fx[0].pluginName == "urn:plug:one");
        }

        // An index that addresses no slot: skipped, and the rest of the file
        // still loads (the `fxin` policy — nothing to attach it to is not a
        // corrupt project).
        CHECK(LoadPatched(fx, "fxstate", "fxstate 7 " + Base64Encode(state) + "\n", b));
        if (b.Tracks().size() == 1 && b.Tracks().front().fx.size() == 1)
            CHECK(b.Tracks().front().fx[0].state.empty());

        // A token that is not base64: dropped, not fatal.
        CHECK(LoadPatched(fx, "fxstate", "fxstate 0 not!base64!\n", b));
        if (b.Tracks().size() == 1 && b.Tracks().front().fx.size() == 1)
            CHECK(b.Tracks().front().fx[0].state.empty());

        // A state line with no token at all: same.
        CHECK(LoadPatched(fx, "fxstate", "fxstate 0\n", b));
        if (b.Tracks().size() == 1 && b.Tracks().front().fx.size() == 1)
            CHECK(b.Tracks().front().fx[0].state.empty());

        // A project saved by THIS build, read by an OLD one: the old parser
        // skips unknown keywords, so nothing here can test it directly — but
        // the guarantee it rests on is that the state is on its own line and
        // the `fx` line is untouched, which the assertions above pin.
        CHECK(Contains(ReadFile(kPath), "fx 10 2 0.25 1 \"urn:plug:one\""));
    }

    std::remove(kPath);
    std::printf("fx_state_io_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
