// Host-buildable tests for the take-name chooser and WavWriter's no-clobber
// rule (plan M0.3).
//
// The defect they guard: takes were named from a counter that restarted at 0
// every session, and WavWriter opened with trunc -- so recording again after
// reopening a project silently overwrote take-1.wav, a file that project's
// clips still referenced. The chooser must skip existing names, and the
// writer must refuse to touch a file that exists.

#include "../src/model/TakeNames.h"
#include "../src/engine/WavWriter.h"
#include "../src/engine/WavSource.h"

#include <cstdio>
#include <cstring>
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

static const char* kDir = "takenames_test_dir";

static std::string InDir(const std::string& name) {
    return std::string(kDir) + "/" + name;
}

static bool Exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

static void WriteBytes(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(text, 1, std::strlen(text), f);
    std::fclose(f);
}

// A small read-back for the sentinel checks (WavSource is for WAVs).
static std::string ReadBytes(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[64];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

static void RemoveDirIfEmpty() {
    for (int n = 1; n <= 12; ++n) {
        char name[48];
        std::snprintf(name, sizeof(name), "take-%d.wav", n);
        std::remove(InDir(name).c_str());
        std::snprintf(name, sizeof(name), "frozen-%d.wav", n);
        std::remove(InDir(name).c_str());
    }
    std::remove(InDir("guard.wav").c_str());
    std::remove(InDir("fresh.wav").c_str());
    ::rmdir(kDir);
}

int main() {
    RemoveDirIfEmpty();
    CHECK(::mkdir(kDir, 0777) == 0);

    // An empty directory gets the first name.
    CHECK(NextFreeWavPath(kDir, "take") == InDir("take-1.wav"));

    // Occupied 1 and 2 -> 3.
    WriteBytes(InDir("take-1.wav"), "one");
    WriteBytes(InDir("take-2.wav"), "two");
    CHECK(NextFreeWavPath(kDir, "take") == InDir("take-3.wav"));

    // Gaps are reused (7 exists, 3 does not).
    WriteBytes(InDir("take-7.wav"), "seven");
    CHECK(NextFreeWavPath(kDir, "take") == InDir("take-3.wav"));

    // The regression itself: "session A" picks a name and writes it; a fresh
    // call -- what a reopened project's first take does, with no counter
    // memory -- must pick a DIFFERENT, free name and leave A's file intact.
    const std::string sessionA = NextFreeWavPath(kDir, "take");
    WriteBytes(sessionA, "session A take");
    const std::string sessionB = NextFreeWavPath(kDir, "take");
    CHECK(sessionB != sessionA);
    CHECK(!Exists(sessionB));
    CHECK(ReadBytes(sessionA) == "session A take");

    // Renders scan the same way, under their own prefix.
    CHECK(NextFreeWavPath(kDir, "frozen") == InDir("frozen-1.wav"));

    // WavWriter's backstop: an exclusive open on an existing file fails AND
    // leaves the file byte-for-byte intact.
    const std::string guard = InDir("guard.wav");
    WriteBytes(guard, "SENTINEL");
    {
        WavWriter w;
        CHECK(!w.Open(guard, 48000, 2, /*exclusive*/ true));
        CHECK(!w.IsOpen());
        CHECK(ReadBytes(guard) == "SENTINEL");
    }

    // ...and a free path still writes a real take through it.
    const std::vector<int16_t> samples = { 0, 0, 16384, -16384 };
    {
        const std::string fresh = InDir("fresh.wav");
        WavWriter w;
        CHECK(w.Open(fresh, 48000, 2, /*exclusive*/ true));
        CHECK(w.WriteInt16(samples.data(), samples.size()));
        CHECK(w.Close());
        WavSource s;
        CHECK(s.Open(fresh));
        CHECK(s.TotalFrames() == 2);
    }

    // Non-exclusive is unchanged: it truncates (the exporter's `.part` temp
    // depends on that).
    {
        WavWriter t;
        CHECK(t.Open(guard, 48000, 2));      // default: trunc
        CHECK(t.WriteInt16(samples.data(), samples.size()));
        CHECK(t.Close());
        WavSource gs;
        CHECK(gs.Open(guard));
        CHECK(gs.IsValid());
        CHECK(ReadBytes(guard) != "SENTINEL");
    }

    // Leave the tree as we found it.
    std::remove(sessionA.c_str());
    std::remove(sessionB.c_str());
    for (int n = 1; n <= 12; ++n) {
        char name[48];
        std::snprintf(name, sizeof(name), "take-%d.wav", n);
        std::remove(InDir(name).c_str());
    }
    std::remove(guard.c_str());
    std::remove(InDir("fresh.wav").c_str());
    ::rmdir(kDir);

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
