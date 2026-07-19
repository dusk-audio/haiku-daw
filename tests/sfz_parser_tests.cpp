// Host-buildable tests for the SFZ parser: opcode scoping, the space-bearing
// sample= value, note-name keys, both spellings of loop_mode/off_by, and
// locale-independent decimal parsing.
//
// Sample files are synthesised into a temp directory so the tests do not depend
// on any installed library.

#include "../src/synth/SfzParser.h"
#include "../src/synth/SampleBank.h"
#include "../src/engine/WavWriter.h"

#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static std::string g_dir;

// Write a `frames`-long mono WAV of a constant value, so a test can tell which
// sample a region picked by looking at the rendered amplitude.
static bool WriteMono(const std::string& name, int64_t frames, float value,
                      int rate = 44100) {
    WavWriter w;
    // 32-bit float so the test's exact sample values survive the round trip.
    if (!w.OpenFormat(g_dir + "/" + name, rate, 1, 32, true))
        return false;
    std::vector<float> buf((size_t)frames, value);
    const bool ok = w.WriteFloat(buf.data(), (size_t)frames, false);
    w.Close();
    return ok;
}

static const Region* FindRegionForKey(const LoadedInstrument& inst, int key) {
    for (const Region& r : inst.regions)
        if (r.MatchesKey(key)) return &r;
    return nullptr;
}

int main() {
    // A temp directory the tests own outright.
    char tmpl[] = "/tmp/sfz_parser_tests_XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) { std::printf("cannot create temp dir\n"); return 1; }
    g_dir = d;

    CHECK(WriteMono("Bd Brush 1.wav", 1000, 0.5f));
    CHECK(WriteMono("snare.wav", 2000, 0.25f));
    CHECK(WriteMono("hat open.wav", 3000, 0.125f));

    // ---- sample= values containing spaces -------------------------------
    // The single hardest part of SFZ lexing: an unquoted value runs until the
    // next opcode name. Real libraries name samples "Bd Brush 1.wav".
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=Bd Brush 1.wav lokey=36 hikey=36 volume=-2 pan=0\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].loKey == 36);
            CHECK(inst.regions[0].hiKey == 36);
            CHECK(std::fabs(inst.regions[0].volumeDb - (-2.0f)) < 1e-6f);
            CHECK(inst.samples.size() == 1);
            CHECK(inst.samples[0].frames == 1000);
        }
    }

    // ---- group -> region inheritance, and region override ---------------
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<group> volume=-6 ampeg_release=0.3 loop_mode=one_shot\n"
            "<region> sample=snare.wav lokey=38 hikey=38\n"
            "<region> sample=hat open.wav lokey=46 hikey=46 volume=3\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 2);
        if (inst.regions.size() == 2) {
            CHECK(std::fabs(inst.regions[0].volumeDb - (-6.0f)) < 1e-6f);  // inherited
            CHECK(std::fabs(inst.regions[1].volumeDb - 3.0f) < 1e-6f);     // overridden
            CHECK(std::fabs(inst.regions[0].ampegRelease - 0.3f) < 1e-6f);
            CHECK(inst.regions[0].loopMode == LoopMode::OneShot);
            CHECK(inst.regions[1].loopMode == LoopMode::OneShot);
        }
    }

    // A second <group> must not leak the first group's opcodes.
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<group> volume=-6\n"
            "<region> sample=snare.wav lokey=38 hikey=38\n"
            "<group> pan=50\n"
            "<region> sample=hat open.wav lokey=46 hikey=46\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 2);
        if (inst.regions.size() == 2) {
            CHECK(std::fabs(inst.regions[1].volumeDb) < 1e-6f);        // not -6
            CHECK(std::fabs(inst.regions[1].pan - 50.0f) < 1e-6f);
            CHECK(std::fabs(inst.regions[0].pan) < 1e-6f);
        }
    }

    // ---- the legacy spellings the Pettinghouse kits actually use --------
    // "loopmode" (no underscore) and "offby" are ARIA-era spellings; a parser
    // that only accepts the v1 names silently loses one-shot behaviour, which
    // makes every drum hit cut off at note-off.
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 loopmode=one_shot group=1 offby=2\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].loopMode == LoopMode::OneShot);
            CHECK(inst.regions[0].group == 1);
            CHECK(inst.regions[0].offBy == 2);
            // key= sets all three at once.
            CHECK(inst.regions[0].loKey == 38);
            CHECK(inst.regions[0].hiKey == 38);
            CHECK(inst.regions[0].pitchKeycenter == 38);
        }
    }

    // ---- note names ------------------------------------------------------
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav lokey=c4 hikey=c#4 pitch_keycenter=a4\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].loKey == 60);
            CHECK(inst.regions[0].hiKey == 61);
            CHECK(inst.regions[0].pitchKeycenter == 69);
        }
    }

    // ---- comments, CRLF, and headers sharing a line with opcodes --------
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "// leading comment\r\n"
            "/* block\n"
            "   comment */ <region> sample=snare.wav key=38 // trailing\r\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
    }

    // ---- locale-independent decimals -------------------------------------
    // strtod follows the C locale's decimal separator. Under a comma locale a
    // locale-dependent parse reads "37.795" as 37, silently mispanning every
    // region in a library. Only assert if the locale is actually available.
    {
        const char* got = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 pan=37.7952755905512 volume=-2.5\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(std::fabs(inst.regions[0].pan - 37.7952755905512f) < 1e-3f);
            CHECK(std::fabs(inst.regions[0].volumeDb - (-2.5f)) < 1e-6f);
        }
        if (!got)
            std::printf("  note: de_DE.UTF-8 not installed; locale check was "
                        "run under \"%s\"\n", std::setlocale(LC_NUMERIC, nullptr));
        std::setlocale(LC_NUMERIC, "C");
    }

    // ---- end=-1 disables a region (it does NOT mean "play to the end") ---
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 end=-1\n"
            "<region> sample=snare.wav key=40 end=500\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].loKey == 40);
            CHECK(inst.regions[0].end == 500);
        }
    }

    // ---- out-of-range spans are clamped into the sample ------------------
    // These come from untrusted files; everything downstream indexes the
    // sample buffer without re-checking.
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 offset=999999 end=888888 "
            "loop_mode=loop_continuous loop_start=-5 loop_end=999999\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            const Region& r = inst.regions[0];
            CHECK(r.offset >= 0 && r.offset < 2000);
            CHECK(r.end >= r.offset && r.end < 2000);
            CHECK(r.loopStart >= r.offset);
            CHECK(r.loopEnd <= r.end);
        }
    }

    // ---- a missing sample drops its region, it does not fail the file ----
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=nope.wav key=38\n"
            "<region> sample=snare.wav key=40\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1)
            CHECK(inst.regions[0].loKey == 40);
    }

    // ---- default_path and #define ---------------------------------------
    {
        std::string sub = g_dir + "/samples";
        CHECK(std::system(("mkdir -p '" + sub + "'").c_str()) == 0);
        {
            const std::string save = g_dir;
            g_dir = sub;
            CHECK(WriteMono("kick.wav", 512, 0.75f));
            g_dir = save;
        }
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<control> default_path=samples/\n"
            "#define $KEY 36\n"
            "<region> sample=kick.wav key=$KEY\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].loKey == 36);
            CHECK(inst.samples.size() == 1);
            CHECK(inst.samples[0].frames == 512);
        }
    }

    // ---- one sample shared by many regions is decoded once ---------------
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 lovel=0 hivel=63\n"
            "<region> sample=snare.wav key=38 lovel=64 hivel=127\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 2);
        CHECK(inst.samples.size() == 1);
        CHECK(FindRegionForKey(inst, 38) != nullptr);
    }

    // ---- HARDENING: exponential #define expansion ("billion laughs") -----
    // Defines whose values reference later-expanded defines double the line per
    // level. A 1 KB file used to take 20 s and climb until memory ran out.
    {
        std::string src;
        std::string name(24, 'a');
        for (int lvl = 0; lvl < 22; lvl++) {
            const std::string next(name.size() - 1, 'a');
            src += "#define $" + name + " $" + next + "$" + next + "\n";
            name = next;
        }
        src += "#define $" + name + " XXXXXXXXXXXXXXXX\n";
        src += "<region> sample=snare.wav key=38\n";

        const auto t0 = std::chrono::steady_clock::now();
        LoadedInstrument inst;
        std::string err;
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        ++g_checks;
        if (ms > 2000.0) {
            ++g_fails;
            std::printf("  FAIL #define expansion took %.0f ms\n", ms);
        }
    }

    // ---- HARDENING: #include fan-out ------------------------------------
    // A depth cap alone doesn't bound the work: a file including itself 12
    // times is parsed 12^9 times. Must terminate via the total-file budget.
    {
        const std::string selfPath = g_dir + "/selfinc.sfz";
        {
            std::ofstream f(selfPath);
            for (int i = 0; i < 12; i++)
                f << "#include \"selfinc.sfz\"\n";
            f << "<region> sample=snare.wav key=38\n";
        }
        const auto t0 = std::chrono::steady_clock::now();
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSfz(selfPath, &inst, &err));
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        ++g_checks;
        if (ms > 5000.0) {
            ++g_fails;
            std::printf("  FAIL self-include took %.0f ms\n", ms);
        }
    }

    // ---- HARDENING: a WAV declaring a huge data chunk ---------------------
    // TotalFrames came from the declared chunk size, so a 48-byte file could
    // claim 4 G frames and make LoadWavToMemory reserve ~17 GB.
    {
        const std::string p = g_dir + "/liar.wav";
        {
            std::ofstream f(p, std::ios::binary);
            auto u32 = [&](uint32_t v) {
                f.put((char)(v & 0xff)); f.put((char)((v >> 8) & 0xff));
                f.put((char)((v >> 16) & 0xff)); f.put((char)((v >> 24) & 0xff));
            };
            auto u16 = [&](uint16_t v) {
                f.put((char)(v & 0xff)); f.put((char)((v >> 8) & 0xff));
            };
            f << "RIFF"; u32(0xFFFFFFFFu); f << "WAVE";
            f << "fmt "; u32(16);
            u16(1); u16(1); u32(44100); u32(44100); u16(1); u16(8);
            f << "data"; u32(0xFFFFFFFFu);
            for (int i = 0; i < 16; i++) f.put((char)128);   // 16 real frames
        }
        SampleData sd;
        // Either it loads the 16 real frames or it refuses — it must not try to
        // allocate the declared 4 G frames.
        const bool ok = LoadWavToMemory(p, &sd);
        ++g_checks;
        if (ok && sd.frames > 1000) {
            ++g_fails;
            std::printf("  FAIL liar.wav yielded %lld frames\n",
                        (long long)sd.frames);
        }
    }

    // ---- HARDENING: absurd channel count ---------------------------------
    {
        const std::string p = g_dir + "/manych.wav";
        {
            std::ofstream f(p, std::ios::binary);
            auto u32 = [&](uint32_t v) {
                f.put((char)(v & 0xff)); f.put((char)((v >> 8) & 0xff));
                f.put((char)((v >> 16) & 0xff)); f.put((char)((v >> 24) & 0xff));
            };
            auto u16 = [&](uint16_t v) {
                f.put((char)(v & 0xff)); f.put((char)((v >> 8) & 0xff));
            };
            f << "RIFF"; u32(0xFFFFFFFFu); f << "WAVE";
            f << "fmt "; u32(16);
            u16(1); u16(65535); u32(44100); u32(44100); u16(4); u16(32);
            f << "data"; u32(0xFFFFFFFFu);
            for (int i = 0; i < 64; i++) f.put((char)0);
        }
        SampleData sd;
        CHECK(!LoadWavToMemory(p, &sd));   // rejected, not a 2 GB allocation
    }

    // ---- REGRESSION: headers and opcodes keep SOURCE ORDER --------------
    // A header-first lexer misplaced every opcode preceding a header on the
    // same line: the group's volume landed on the region instead.
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<group> volume=-6 <region> sample=snare.wav key=38 "
            "<region> sample=hat open.wav key=46 volume=3\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 2);
        if (inst.regions.size() == 2) {
            CHECK(std::fabs(inst.regions[0].volumeDb - (-6.0f)) < 1e-6f);
            CHECK(inst.regions[0].loKey == 38);
            CHECK(std::fabs(inst.regions[1].volumeDb - 3.0f) < 1e-6f);
            CHECK(inst.regions[1].loKey == 46);
        }
    }
    {
        // The value-with-spaces rule must still hold when a header follows.
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=Bd Brush 1.wav key=36 <region> sample=snare.wav key=38\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 2);
        CHECK(inst.samples.size() == 2);
    }

    // ---- REGRESSION: saturating integer parses (were signed-overflow UB) --
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=snare.wav key=38 offset=99999999999999999999999 "
            "tune=1e999999999999999999 loop_start=99999999999999999999\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.size() == 1);
        if (inst.regions.size() == 1) {
            // Everything is clamped into the sample by FinalizeInstrument.
            const Region& r = inst.regions[0];
            CHECK(r.offset >= 0 && r.offset < 2000);
            CHECK(std::isfinite(r.tuneCents));
        }
    }

    // ---- a Windows drive-absolute path is not glued onto baseDir ---------
    {
        LoadedInstrument inst;
        std::string err;
        const std::string src =
            "<region> sample=C:\\Samples\\nope.wav key=38\n";
        // It cannot resolve on this host either way; the point is that it is
        // not silently turned into "<baseDir>/C:/Samples/nope.wav".
        CHECK(ParseSfzText(src, g_dir, &inst, &err));
        CHECK(inst.regions.empty());
    }

    // ---- REGRESSION: a partial load must REPORT itself ------------------
    // Two ways this was silently lost: LoadSoundfont discarded the loader's
    // warning outright, and later the budget preflight made "does not fit"
    // return the same failure as "file missing", so the over-budget flag never
    // got set. Either way a half-loaded kit looked complete.
    //
    // A missing sample must NOT be mistaken for a budget stop.
    {
        LoadedInstrument inst;
        std::string err, warn;
        const std::string src =
            "<region> sample=gone_a.wav key=36\n"
            "<region> sample=gone_b.wav key=38\n"
            "<region> sample=snare.wav key=40\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err, &warn));
        CHECK(inst.regions.size() == 1);   // only the real one survives
        CHECK(warn.empty());               // missing files are not a budget stop
    }
    // A kit that fits reports no warning at all.
    {
        LoadedInstrument inst;
        std::string err, warn;
        const std::string src = "<region> sample=snare.wav key=38\n";
        CHECK(ParseSfzText(src, g_dir, &inst, &err, &warn));
        CHECK(inst.regions.size() == 1);
        CHECK(warn.empty());
        CHECK(inst.warning.empty());
    }
    // And the whole path end-to-end: LoadSoundfont must hand the warning back,
    // and a CACHE HIT must still report it.
    {
        const std::string p = g_dir + "/tiny.sfz";
        { std::ofstream f(p); f << "<region> sample=snare.wav key=38\n"; }
        std::string err, warn;
        auto a1 = SoundfontCache::Instance().Load(p, 0, &err, &warn);
        CHECK(a1 != nullptr);
        CHECK(warn.empty());
        std::string warn2;
        auto a2 = SoundfontCache::Instance().Load(p, 0, &err, &warn2);
        CHECK(a2 == a1);                   // served from the cache
        CHECK(warn2 == a1->warning);       // a hit reports what the load did
    }

    std::system(("rm -rf '" + g_dir + "'").c_str());

    std::printf("sfz_parser_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
