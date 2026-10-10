// Host tests for AppSettings text (de)serialization round-trip.
#include "../src/app/AppSettings.h"

#include <cmath>
#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    AppSettings a;
    a.bufferFrames = 1024;
    a.countInBars  = 2;
    a.metronome    = true;
    a.monitorInput = true;
    a.lastDir      = "/boot/home/My Samples";   // note the space
    a.winL = 10; a.winT = 20; a.winR = 900; a.winB = 640;
    a.exportBitDepth     = 24;
    a.exportDither       = false;
    a.exportSampleRate   = 96000;
    a.exportNormalize    = true;
    a.exportTargetLufs   = -16.5f;
    a.exportTruePeakCeil = -1.5f;
    a.exportLimiter      = true;
    a.exportRange        = 1;
    a.exportStems        = 1;
    a.inspectorVisible   = false;      // the M1.4 panes
    a.inspectorWidth     = 240.0f;
    a.bottomVisible      = true;
    a.bottomHeight       = 300.0f;

    AppSettings b;
    CHECK(b.Deserialize(a.Serialize()));
    CHECK(b.bufferFrames == 1024);
    CHECK(b.countInBars == 2);
    CHECK(b.metronome == true);
    CHECK(b.monitorInput == true);
    CHECK(b.lastDir == "/boot/home/My Samples");
    CHECK(std::fabs(b.winL - 10) < 1e-3 && std::fabs(b.winR - 900) < 1e-3);
    // The export dialog's last choices survive too (they are the reason the
    // dialog does not have to be re-specified on every bounce).
    CHECK(b.exportBitDepth == 24);
    CHECK(b.exportDither == false);
    CHECK(b.exportSampleRate == 96000);
    CHECK(b.exportNormalize == true);
    CHECK(std::fabs(b.exportTargetLufs - (-16.5f)) < 1e-3);
    CHECK(std::fabs(b.exportTruePeakCeil - (-1.5f)) < 1e-3);
    CHECK(b.exportLimiter == true);
    CHECK(b.exportRange == 1);
    CHECK(b.exportStems == 1);
    // The pane layout the user left behind (M1.4).
    CHECK(b.inspectorVisible == false);
    CHECK(std::fabs(b.inspectorWidth - 240.0f) < 1e-3);
    CHECK(b.bottomVisible == true);
    CHECK(std::fabs(b.bottomHeight - 300.0f) < 1e-3);

    // The dock's page (T2).
    {
        AppSettings d;
        d.dockPage = 2;
        AppSettings d2;
        CHECK(d2.Deserialize(d.Serialize()));
        CHECK(d2.dockPage == 2);
        AppSettings plain;
        CHECK(plain.Deserialize("buffer 512\n"));
        CHECK(plain.dockPage == 0);
    }

    // The look (T1): the mode survives, and a file that has no theme line --
    // or a hand-edited one -- stays on System, which is the mode the app is
    // allowed to draw anywhere.
    {
        AppSettings t;
        t.themeMode = ThemeMode::Dark;
        AppSettings t2;
        CHECK(t2.Deserialize(t.Serialize()));
        CHECK(t2.themeMode == ThemeMode::Dark);

        AppSettings plain;
        CHECK(plain.Deserialize("buffer 512\n"));
        CHECK(plain.themeMode == ThemeMode::System);

        AppSettings odd;
        CHECK(odd.Deserialize("theme midnight\n"));
        CHECK(odd.themeMode == ThemeMode::System);
        CHECK(odd.Deserialize("theme dark\n"));
        CHECK(odd.themeMode == ThemeMode::Dark);
    }

    // An old settings file (no export keys at all) keeps the dialog defaults.
    AppSettings old;
    CHECK(old.Deserialize("buffer 512\nmetronome 0\n"));
    CHECK(old.exportBitDepth == 16 && old.exportDither == true);
    CHECK(old.exportSampleRate == 0 && old.exportNormalize == false);
    CHECK(old.exportRange == 0 && old.exportStems == 0);
    // ...and the panes default to the inspector showing, the dock hidden.
    CHECK(old.inspectorVisible == true && old.bottomVisible == false);

    // Garbage / empty -> false, defaults retained.
    AppSettings c;
    CHECK(!c.Deserialize(""));
    CHECK(c.bufferFrames == 512);

    // Unknown keys ignored; known ones still parsed.
    AppSettings d;
    CHECK(d.Deserialize("mystery 5\nbuffer 256\n"));
    CHECK(d.bufferFrames == 256);

    // Regression: a malformed numeric value must keep the default (not zero it,
    // which would make buffer=0 -> div-by-zero in the audio path).
    AppSettings e;
    CHECK(e.Deserialize("buffer notanumber\ncountin 1\n"));
    CHECK(e.bufferFrames == 512);   // default retained
    CHECK(e.countInBars == 1);      // the good line still parsed

    // --- the recent-projects list (M0.2) --------------------------------
    {
        // Round trip, order kept, spaces survive.
        AppSettings r;
        r.recentProjects = { "/boot/home/My Songs/one.dawproj",
                             "/tmp/two.dawproj" };
        AppSettings r2;
        CHECK(r2.Deserialize(r.Serialize()));
        CHECK(r2.recentProjects.size() == 2);
        CHECK(r2.recentProjects[0] == "/boot/home/My Songs/one.dawproj");
        CHECK(r2.recentProjects[1] == "/tmp/two.dawproj");

        // RememberRecent: front, deduplicated, capped.
        AppSettings::RememberRecent(r2.recentProjects, "/tmp/two.dawproj");
        CHECK(r2.recentProjects.size() == 2);           // no duplicate
        CHECK(r2.recentProjects[0] == "/tmp/two.dawproj");

        std::vector<std::string> many;
        for (int i = 0; i < 14; i++)
            AppSettings::RememberRecent(many,
                                        "/tmp/p" + std::to_string(i) + ".dawproj");
        CHECK(many.size() == AppSettings::kMaxRecent);  // capped
        CHECK(many[0] == "/tmp/p13.dawproj");           // newest first
        CHECK(many[AppSettings::kMaxRecent - 1] == "/tmp/p4.dawproj");

        // An empty path changes nothing.
        AppSettings::RememberRecent(many, "");
        CHECK(many.size() == AppSettings::kMaxRecent);

        // A parse is the whole state: parsing into a used object replaces the
        // list instead of appending to it.
        AppSettings r3;
        r3.recentProjects = { "/tmp/stale.dawproj" };
        CHECK(r3.Deserialize("recent /tmp/fresh.dawproj\n"));
        CHECK(r3.recentProjects.size() == 1);
        CHECK(r3.recentProjects[0] == "/tmp/fresh.dawproj");

        // A file with more than the cap keeps only the first kMaxRecent lines.
        std::string fat;
        for (int i = 0; i < 15; i++)
            fat += "recent /tmp/q" + std::to_string(i) + ".dawproj\n";
        AppSettings r4;
        CHECK(r4.Deserialize(fat));
        CHECK(r4.recentProjects.size() == AppSettings::kMaxRecent);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
