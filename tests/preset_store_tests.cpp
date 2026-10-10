// Host-buildable tests for the DAW's preset files (Lv2PresetStore.h).
//
// The format is ours: one file per preset, the plugin URI inside it rather than
// in the name, base64 for the plugin's opaque state. What has to hold: a saved
// preset loads back exactly, presets for different plugins share one directory
// without shadowing each other, a name that could escape the directory does not,
// and a torn or foreign file is skipped rather than half-loaded. Kit-free — no
// lilv anywhere in this test.

#include "../src/plugin/Lv2PresetStore.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
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

static std::string Dir() {
    return std::string("/tmp/daw_preset_store_test_") + std::to_string(::getpid());
}

static void RemoveDir(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d) {
        while (dirent* e = ::readdir(d)) {
            const std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::remove((dir + "/" + n).c_str());
        }
        ::closedir(d);
    }
    ::rmdir(dir.c_str());
}

static PresetEntry MakeEntry(const std::string& name, const std::string& state,
                             const std::vector<float>& params) {
    PresetEntry e;
    e.name = name;
    e.state = state;
    e.params = params;
    return e;
}

int main() {
    const std::string dir = Dir();
    RemoveDir(dir);   // a leftover from a killed run

    const std::string uriA = "urn:haiku-daw:test:mono-gain";
    const std::string uriB = "urn:haiku-daw:test:stateful";

    // ---- round trip --------------------------------------------------------
    {
        const std::string state = "@prefix p: <urn:x:> .\n<x> p:y \"a \\\"q\\\"\" .\n";
        CHECK(SavePresetToStore(dir, uriA, MakeEntry("Bright Pad", state, { 0.5f, 2.0f, -1.0f })));

        const std::vector<PresetEntry> list = LoadPresetsFor(dir, uriA);
        CHECK(list.size() == 1);
        if (list.size() == 1) {
            CHECK(list[0].name == "Bright Pad");
            CHECK(list[0].state == state);
            CHECK(list[0].params.size() == 3);
            if (list[0].params.size() == 3) {
                CHECK(list[0].params[0] == 0.5f);
                CHECK(list[0].params[1] == 2.0f);
                CHECK(list[0].params[2] == -1.0f);
            }
        }
        // ...and it is invisible to another plugin sharing the directory.
        CHECK(LoadPresetsFor(dir, uriB).empty());
    }

    // ---- two plugins, same preset name, one directory ----------------------
    {
        CHECK(SavePresetToStore(dir, uriB, MakeEntry("Bright Pad", "B state", { 7.0f })));
        const std::vector<PresetEntry> a = LoadPresetsFor(dir, uriA);
        const std::vector<PresetEntry> b = LoadPresetsFor(dir, uriB);
        CHECK(a.size() == 1 && a[0].state != "B state");
        CHECK(b.size() == 1);
        if (b.size() == 1) {
            CHECK(b[0].name == "Bright Pad");
            CHECK(b[0].state == "B state");
        }
    }

    // ---- a second preset for the same plugin -------------------------------
    {
        CHECK(SavePresetToStore(dir, uriA, MakeEntry("Dark", "", { 0.1f })));
        const std::vector<PresetEntry> list = LoadPresetsFor(dir, uriA);
        CHECK(list.size() == 2);
        // An empty state is legal (a preset of parameter values on a plugin with
        // no internal state) and comes back as an empty state, not a failure.
        bool sawDark = false;
        for (const PresetEntry& e : list)
            if (e.name == "Dark") { sawDark = true; CHECK(e.state.empty()); CHECK(e.params.size() == 1); }
        CHECK(sawDark);
    }

    // ---- names that are paths, and names that repeat -----------------------
    {
        CHECK(SavePresetToStore(dir, uriA, MakeEntry("../../escape", "x", {})));
        CHECK(SavePresetToStore(dir, uriA, MakeEntry("../../escape", "y", {})));
        const std::vector<PresetEntry> list = LoadPresetsFor(dir, uriA);
        CHECK(list.size() == 4);   // nothing escaped into the parent
        struct stat st;
        CHECK(::stat("/tmp/escape.dawpreset", &st) != 0);

        // Both copies of the repeated name are there, with their own state —
        // the second got its own file rather than overwriting the first.
        int copies = 0;
        for (const PresetEntry& e : list)
            if (e.name == "../../escape") { copies++; CHECK(e.state == "x" || e.state == "y"); }
        CHECK(copies == 2);
    }

    // ---- torn and foreign files are skipped --------------------------------
    {
        const std::string junk = dir + "/junk.dawpreset";
        { std::FILE* f = std::fopen(junk.c_str(), "wb"); std::fputs("not a preset\x01\x02", f); std::fclose(f); }
        const std::string badstate = dir + "/badstate.dawpreset";
        { std::FILE* f = std::fopen(badstate.c_str(), "wb");
          std::fputs("DAWPRESET 1\nplugin " , f); std::fputs(uriA.c_str(), f);
          std::fputs("\nname Broken\nstate @@@notbase64@@@\nparams 1 0.5\n", f); std::fclose(f); }
        const std::string truncated = dir + "/trunc.dawpreset";
        { std::FILE* f = std::fopen(truncated.c_str(), "wb");
          std::fputs("DAWPRESET 1\nplugin ", f); std::fputs(uriA.c_str(), f);
          std::fputs("\nname NoState\n", f); std::fclose(f); }   // no state line
        const std::string notours = dir + "/readme.txt";
        { std::FILE* f = std::fopen(notours.c_str(), "wb"); std::fputs("hello", f); std::fclose(f); }

        const std::vector<PresetEntry> list = LoadPresetsFor(dir, uriA);
        CHECK(list.size() == 4);   // the four real ones, none of the four above
        for (const PresetEntry& e : list) CHECK(e.name != "Broken" && e.name != "NoState");
    }

    // ---- an unwritable / missing directory is a failure, not a crash -------
    {
        CHECK(!SavePresetToStore("", uriA, MakeEntry("x", "y", {})));
        CHECK(!SavePresetToStore(dir, "", MakeEntry("x", "y", {})));
        CHECK(!SavePresetToStore(dir, uriA, MakeEntry("", "y", {})));
        CHECK(LoadPresetsFor(dir + "/does-not-exist", uriA).empty());
    }

    // ---- the parse itself ---------------------------------------------------
    {
        PresetEntry e;
        CHECK(ParsePreset(SerializePreset(uriA, MakeEntry("N", "S", { 1.0f, 2.0f })), uriA, &e));
        CHECK(e.name == "N" && e.state == "S" && e.params.size() == 2);
        CHECK(!ParsePreset(SerializePreset(uriA, MakeEntry("N", "S", {})), uriB, &e));
        CHECK(!ParsePreset("", uriA, &e));
        CHECK(!ParsePreset("DAWPRESET 2\nplugin urn:x\nname N\nstate \n", uriA, &e));
        // A future build's extra key is skipped, not fatal.
        CHECK(ParsePreset("DAWPRESET 1\nplugin " + uriA + "\nname N\nfuture 1 2 3\nstate \n", uriA, &e));
        // CR line endings survive (a preset copied through a text-mode tool).
        CHECK(ParsePreset("DAWPRESET 1\r\nplugin " + uriA + "\r\nname N\r\nstate \r\n", uriA, &e));
    }

    RemoveDir(dir);
    std::printf("preset_store_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
