// Adversarial fuzz corpus for the project loader. A truncated, count-corrupted,
// or garbage .dawproj must never crash, hang, or drive an unbounded allocation,
// and a FAILED load must leave the caller's existing project untouched (the
// loader parses into a temp and swaps only on a clean parse). Deterministic
// PRNG; build -DDAW_SANITIZE=ON to catch silent OOB under ASan/UBSan.

#include "../src/model/ProjectIO.h"
#include "../src/model/Commands.h"

#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Build a representative project and serialize it to a text blob we then corrupt.
static std::string baseProject() {
    Project a;
    CommandStack s;
    a.sampleRate = 44100.0; a.tempoBPM = 120.0; a.masterGain = 0.8f;
    a.masterFx.push_back(ReverbDesc(0.5f, 0.3f));
    s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr"), a);
    TrackId t1 = a.Tracks().front().id;
    Clip c; c.startFrame = 1000; c.lengthFrames = 2000; c.sourcePath = "m/a.wav";
    s.Execute(std::make_unique<AddClipCommand>(t1, c), a);
    s.Execute(std::make_unique<AddEffectCommand>(t1, LowPassDesc(800.0f)), a);
    { AutomationLane g; g.AddPoint(0, 1.0f); g.AddPoint(48000, 0.2f);
      s.Execute(std::make_unique<SetAutoLaneCommand>(t1, AutoLaneKind::Gain, g), a); }
    s.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Syn"), a);
    TrackId t2 = a.Tracks().back().id;
    MidiClip mc; mc.startFrame = 0; mc.lengthFrames = 4000;
    { MidiNote n; n.pitch = 60; n.velocity = 100; n.startFrame = 0;
      n.lengthFrames = 480; mc.notes.push_back(n); }
    s.Execute(std::make_unique<AddMidiClipCommand>(t2, mc), a);
    s.Execute(std::make_unique<AddMarkerCommand>(1000, "A"), a);

    const char* tmp = "projfuzz_base_tmp.dawproj";
    if (!ProjectIO::Save(a, tmp)) return "";
    std::ifstream f(tmp);
    std::stringstream ss; ss << f.rdbuf();
    f.close();
    std::remove(tmp);
    return ss.str();
}

// Write `text` to disk and try to load it into a project that already holds a
// track. Whatever happens must not crash/hang; on failure the project is intact.
static void probe(const std::string& text) {
    const char* path = "projfuzz_tmp.dawproj";
    { std::ofstream f(path); f << text; }

    Project keep;
    CommandStack ks;
    ks.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "KEEP"), keep);

    const bool ok = ProjectIO::Load(keep, path);
    if (!ok) {
        // Rejected load must not have disturbed the existing project.
        CHECK(keep.Tracks().size() == 1);
        CHECK(keep.Tracks().front().name == "KEEP");
    }
    std::remove(path);
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out; std::string line; std::stringstream ss(s);
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}
static std::string join(const std::vector<std::string>& lines) {
    std::string s; for (const auto& l : lines) { s += l; s += '\n'; } return s;
}

int main() {
    const std::string base = baseProject();
    if (base.empty()) {   // fatal: everything below indexes into `base`
        std::printf("  FATAL %s: baseProject() failed to serialize\n", __FILE__);
        return 1;
    }
    const std::vector<std::string> baseLines = splitLines(base);

    // 1. Clean base loads (sanity).
    probe(base);

    // 2. Truncate after each line (interrupted/partial write).
    for (size_t n = 0; n <= baseLines.size(); n++) {
        std::vector<std::string> l(baseLines.begin(), baseLines.begin() + n);
        probe(join(l));
    }

    // 3. Truncate mid-line at each byte (partial final line, no trailer).
    for (size_t n = 0; n <= base.size(); n += 7)
        probe(base.substr(0, n));

    // 4. Corrupt every numeric token on every line with adversarial values:
    //    huge counts (OOM/hang bait), negatives, and non-numeric junk.
    const char* poisons[] = { "2000000000", "-1", "999999999999999",
                              "4294967296", "NaN", "0x10", "" };
    for (size_t li = 0; li < baseLines.size(); li++) {
        std::stringstream ss(baseLines[li]);
        std::vector<std::string> toks; std::string tk;
        while (ss >> tk) toks.push_back(tk);
        for (size_t ti = 0; ti < toks.size(); ti++) {
            for (const char* poison : poisons) {
                std::vector<std::string> lines = baseLines;
                std::vector<std::string> t = toks;
                t[ti] = poison;
                std::string rebuilt;
                for (size_t k = 0; k < t.size(); k++) {
                    if (k) rebuilt += ' ';
                    rebuilt += t[k];
                }
                lines[li] = rebuilt;
                probe(join(lines));
            }
        }
    }

    // 5. Duplicate ids: a SYNTACTICALLY VALID project carrying repeated track
    //    (and clip) ids must load without aliasing — the loader either rejects it
    //    or produces a project with no two tracks/clips sharing an identity
    //    (AddTrack drops a dup-id track, taking its clips with it).
    {
        const std::string dup =
            "DAW 1 1\nsampleRate 48000\n"
            "track 1 audio 1 0 0 0 0\nclip 5 0 100 0 0 0\n"
            "track 1 audio 1 0 0 0 0\nclip 5 200 100 0 0 0\n"   // same track+clip id
            "enddaw\n";
        const char* dpath = "projfuzz_dup_tmp.dawproj";
        { std::ofstream f(dpath); f << dup; }
        Project d;
        const bool ok = ProjectIO::Load(d, dpath);
        std::remove(dpath);
        if (ok) {
            for (size_t i = 0; i < d.Tracks().size(); i++)
                for (size_t j = i + 1; j < d.Tracks().size(); j++)
                    CHECK(d.Tracks()[i].id != d.Tracks()[j].id);   // no dup track id
            std::set<ClipId> seen;
            for (const Track& t : d.Tracks())
                for (const Clip& c : t.clips) {
                    CHECK(seen.find(c.id) == seen.end());          // no dup clip id
                    seen.insert(c.id);
                }
        }
    }

    // 6. Random line-level shuffling + injection of garbage lines. Indices are
    //    taken modulo the CURRENT line count each op, since deletes/inserts
    //    resize the vector as we go.
    std::mt19937 rng(0xBADF00D);
    for (int iter = 0; iter < 2000; iter++) {
        std::vector<std::string> lines = baseLines;
        const int ops = 1 + (iter % 5);
        for (int k = 0; k < ops; k++) {
            const size_t sz = lines.size();
            const int op = iter % 3;
            if (op == 0 && sz > 0) {                          // delete a line
                lines.erase(lines.begin() + rng() % sz);
            } else if (op == 1) {                             // inject garbage
                lines.insert(lines.begin() + (sz ? rng() % sz : 0),
                             "track 999 badtype 9e9 x x x x");
            } else if (sz > 0) {                              // swap two lines
                std::swap(lines[rng() % sz], lines[rng() % sz]);
            }
        }
        probe(join(lines));
    }

    // 7. Random byte-flip fuzzing of the whole blob (deterministic seed).
    std::uniform_int_distribution<size_t> bpos(0, base.size() ? base.size()-1 : 0);
    std::uniform_int_distribution<int> bval(0, 255);
    for (int iter = 0; iter < 3000; iter++) {
        std::string v = base;
        const int flips = 1 + (iter % 8);
        for (int k = 0; k < flips; k++) v[bpos(rng)] = (char)bval(rng);
        probe(v);
    }

    std::printf("\n%d checks, %d failures (no crash/hang/OOM == pass)\n",
                g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
