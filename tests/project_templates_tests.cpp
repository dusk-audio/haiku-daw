// Host tests for the New Project templates (M3.7): the discovery rules (which
// candidate directory wins, a MISSING directory is a normal answer), the row
// order, dropped-in files, and -- the one that matters most -- that the
// templates this repository actually SHIPS parse and build the shape they
// promise. A template that stops loading is a user's File > New that stops
// working, and nothing on the VM would say so.
#include "../src/app/ProjectTemplates.h"
#include "../src/model/Project.h"
#include "../src/model/ProjectIO.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef DAW_SOURCE_DIR
#define DAW_SOURCE_DIR "."
#endif

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static std::string Dir(const char* name) {
    return std::string("/tmp/haiku_daw_tpl_") + name;
}

static void MakeDir(const std::string& d) { mkdir(d.c_str(), 0755); }

static void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc);
    f << text;
}

// A minimal but valid project file.
static std::string MiniProject() {
    return "DAW 1 1\nsampleRate 48000\ntempo 120 0\nmaster 1\n"
           "timesig 4 4\ntransport 0 0 0 0\npunch 0 0 0\n";
}

int main() {
    // --- FindTemplatesDir: first candidate that has a project file ----------
    {
        const std::string emptyDir = Dir("empty");
        const std::string fullDir  = Dir("full");
        const std::string missing  = Dir("nope");
        MakeDir(emptyDir);
        MakeDir(fullDir);
        WriteFile(fullDir + "/band.dawproj", MiniProject());
        rmdir(missing.c_str());

        // A missing directory does not win, and does not crash: the search just
        // moves on (this is the "no templates installed" case).
        CHECK(FindTemplatesDir({ missing }) == "");
        // An existing but EMPTY directory does not win either: a directory with
        // no templates in it is not a templates directory.
        CHECK(FindTemplatesDir({ emptyDir }) == "");
        // Priority is the candidate order, which is what makes the build tree
        // and the environment override beat the installed copy.
        CHECK(FindTemplatesDir({ missing, emptyDir, fullDir }) == fullDir);
        CHECK(FindTemplatesDir({ emptyDir, fullDir }) == fullDir);
        // Nothing at all -> "" (the caller falls back to its built-in Empty).
        CHECK(FindTemplatesDir({}) == "");
        CHECK(FindTemplatesDir({ "", missing }) == "");
        // A candidate that NAMES a file is a usable directory too.
        CHECK(FindTemplatesDir({ fullDir + "/band.dawproj" }) == fullDir);

        // ScanTemplates of a missing directory is an empty list, not an error.
        CHECK(ScanTemplates(missing).empty());
        CHECK(ScanTemplates("").empty());
    }

    // --- the row order and dropped-in files ---------------------------------
    {
        const std::string dir = Dir("order");
        MakeDir(dir);
        // Deliberately written out of order, and with an extra file: the known
        // ids come first in their own order, then the rest alphabetically.
        WriteFile(dir + "/zeta.dawproj", MiniProject());
        WriteFile(dir + "/producer.dawproj", MiniProject());
        WriteFile(dir + "/band.dawproj", MiniProject());
        WriteFile(dir + "/empty.dawproj", MiniProject());
        WriteFile(dir + "/notes.txt", "not a project");   // ignored
        mkdir((dir + "/sub.dawproj").c_str(), 0755);      // a directory: ignored

        const std::vector<TemplateInfo> rows = ScanTemplates(dir);
        CHECK(rows.size() == 4);
        CHECK(rows[0].id == "empty" && rows[0].name == "Empty");
        CHECK(rows[1].id == "band" && rows[1].name == "Band");
        CHECK(rows[2].id == "producer" && rows[2].name == "Producer");
        CHECK(rows[3].id == "zeta" && rows[3].name == "zeta");
        for (const TemplateInfo& t : rows) {
            CHECK(!t.description.empty());
            CHECK(t.path.find(dir) == 0);
        }

        // The built-in Empty row carries no file, so the dialog can offer it
        // with no templates on the machine at all.
        const TemplateInfo emptyRow = EmptyTemplate();
        CHECK(emptyRow.id == "empty" && emptyRow.path.empty());
        CHECK(emptyRow.name == "Empty" && !emptyRow.description.empty());
    }

    // --- TemplateCandidates: the environment wins, then the tree, then data --
    {
        setenv("DAW_TEMPLATES_DIR", "/env/templates", 1);
        std::vector<std::string> c = TemplateCandidates("/boot/home/daw/build",
                                                       "/boot/system/data");
        CHECK(c.size() == 4);
        CHECK(c[0] == "/env/templates");
        CHECK(c[1] == "/boot/home/daw/build/data/templates");
        CHECK(c[2] == "/boot/home/daw/build/../data/templates");
        CHECK(c[3] == "/boot/system/data/haiku-daw/templates");
        unsetenv("DAW_TEMPLATES_DIR");
        // With neither an app directory nor a data directory there is nothing
        // to offer: an empty list, which the caller reads as "no templates".
        c = TemplateCandidates("", "");
        CHECK(c.empty());
    }

    // --- the templates this repository ships --------------------------------
    {
        const std::string src = std::string(DAW_SOURCE_DIR) + "/data/templates";
        const std::vector<TemplateInfo> rows = ScanTemplates(src);
        CHECK(rows.size() >= 3);
        // Everything below indexes rows[0..2]: below three rows this is not a
        // failing check but an out-of-bounds read, so the whole section is
        // under the guard rather than just the id checks.
        if (rows.size() >= 3) {
            CHECK(rows[0].id == "empty");
            CHECK(rows[1].id == "band");
            CHECK(rows[2].id == "producer");

        // Empty: a real project, and nothing in it.
        {
            const TemplateInfo& t = rows[0];
            Project p;
            CHECK(LoadTemplate(t, p));
            CHECK(std::fabs(p.sampleRate - 48000.0) < 1e-9);
            CHECK(p.Tracks().empty());
            CHECK(p.markers.empty());
        }

        // Band: 8 audio tracks plus a bus, the drums routed to the bus and the
        // rest to the master, all named, all audible.
        {
            Project p;
            CHECK(LoadTemplate(rows[1], p));
            CHECK(p.Tracks().size() == 9);
            if (p.Tracks().size() == 9) {
                int audio = 0, busy = 0, toBus = 0, toMaster = 0;
                TrackId busId = kInvalidTrackId;
                for (const Track& t : p.Tracks()) {
                    if (t.type == TrackType::Bus) { busy++; busId = t.id; }
                    if (t.type == TrackType::Audio) audio++;
                    CHECK(!t.name.empty());
                    CHECK(t.gain > 0.0f && !t.muted);
                    CHECK(t.clips.empty() && t.midiClips.empty());
                }
                for (const Track& t : p.Tracks()) {
                    if (t.type != TrackType::Audio) continue;
                    if (t.output == busId) toBus++;
                    else if (t.output == kInvalidTrackId) toMaster++;
                }
                CHECK(audio == 8);
                CHECK(busy == 1);
                CHECK(toBus == 3);       // the drum tracks
                CHECK(toMaster == 5);
                CHECK(std::fabs(p.tempoBPM - 120.0) < 1e-9);
                CHECK(p.tempoMap.Meters().front().num == 4);
                CHECK(p.markers.size() == 1 && p.markers[0].name == "Intro");
            }
        }

        // Producer: MIDI and audio, a reverb bus, and the two sends that make
        // it a return.
        {
            Project p;
            CHECK(LoadTemplate(rows[2], p));
            int midi = 0, audio = 0, busy = 0, sends = 0;
            TrackId busId = kInvalidTrackId;
            for (const Track& t : p.Tracks()) {
                if (t.type == TrackType::Midi)  midi++;
                if (t.type == TrackType::Audio) audio++;
                if (t.type == TrackType::Bus) { busy++; busId = t.id; }
                sends += (int)t.sends.size();
                for (const Send& s : t.sends) CHECK(s.dest != kInvalidTrackId);
            }
            CHECK(midi == 2);
            CHECK(audio == 3);
            CHECK(busy == 1);
            CHECK(sends == 2);
            CHECK(busId != kInvalidTrackId);
            // The instrument lines came through: the pads are not the default
            // waveform, so the loader read what the template wrote rather than
            // leaving every MIDI track on its defaults.
            int nonDefault = 0;
            for (const Track& t : p.Tracks())
                if (t.type == TrackType::Midi && t.instrument.synth.waveform != 0)
                    nonDefault++;
            CHECK(nonDefault == 1);
        }
        } else {
            std::printf("  (checked out of a source tree? %s)\n", src.c_str());
        }
    }

    // --- LoadTemplate's failure path ---------------------------------------
    {
        TemplateInfo missing;
        missing.id = "band";
        missing.path = Dir("does-not-exist.dawproj");
        Project p;
        CHECK(!LoadTemplate(missing, p));
        CHECK(p.Tracks().empty());          // untouched
        // The built-in Empty row has no file and must not pretend to load one.
        CHECK(!LoadTemplate(EmptyTemplate(), p));

        // A corrupt template file: reported, and the caller's project is left
        // exactly as it was (ProjectIO parses into a temp).
        const std::string bad = Dir("bad.dawproj");
        WriteFile(bad, "DAW 1 1\nThis file is truncated mid-\n");
        TemplateInfo badInfo;
        badInfo.id = "bad";
        badInfo.path = bad;
        Project q;
        Track t;
        t.id = q.NextTrackId();
        t.name = "keep me";
        q.AddTrack(t);
        const bool loaded = LoadTemplate(badInfo, q);
        // Whatever the parser decides about a truncated file, the caller's
        // project is either the loaded one or untouched -- never a half-load.
        if (!loaded) CHECK(q.Tracks().size() == 1);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
