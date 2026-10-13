// Host tests for AppSettings text (de)serialization round-trip.
#include "../src/app/AppSettings.h"

#include <cmath>
#include <cstdio>
#include <string>

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
    a.exportContainer    = 2;          // FLAC, as AudioFileFormat's ordinal
    a.exportBitDepth     = 24;
    a.exportVorbisQuality = 0.8f;
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
    a.takeFormat         = 2;          // 32-bit float takes (M5)

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
    CHECK(b.exportContainer == 2);              // the format survives too
    CHECK(b.exportBitDepth == 24);
    CHECK(std::fabs(b.exportVorbisQuality - 0.8f) < 1e-3);
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
    // The take sample format (M5), and the pre-M5 default for a file that has
    // no key at all: 24-bit PCM.
    CHECK(b.takeFormat == 2);
    {
        AppSettings legacy;
        CHECK(legacy.Deserialize("buffer 512\n"));
        CHECK(legacy.takeFormat == 1);
    }

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
    // A file an older build wrote has no container line: the dialog must
    // reopen on WAV, not on whatever the enum's zero happens to be.
    CHECK(old.exportContainer == 0 && old.exportVorbisQuality == 0.5f);
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

    // --- the marker lane (M2.6) ------------------------------------------
    {
        // Off by default, round-trips, and a settings file from before the
        // preference keeps the default.
        AppSettings m;
        CHECK(m.markerLaneVisible == false);
        m.markerLaneVisible = true;
        AppSettings m2;
        CHECK(m2.Deserialize(m.Serialize()));
        CHECK(m2.markerLaneVisible == true);

        AppSettings m3;
        CHECK(m3.Deserialize("buffer 256\nuimarklane 1\n"));
        CHECK(m3.markerLaneVisible == true);

        AppSettings m4;
        CHECK(m4.Deserialize("buffer 256\n"));   // older file: no line
        CHECK(m4.markerLaneVisible == false);
    }

    // --- the Preferences window's keys (M3.5) ----------------------------
    {
        // Every one round-trips, spaces in the folder path included.
        AppSettings p;
        p.defaultSampleRate = 96000;
        p.metronomeSound    = 2;
        p.metronomeLevel    = 0.75f;
        p.uiScale           = 1.5f;
        p.projectDir        = "/boot/home/My Projects";
        p.autosaveSeconds   = 300;
        AppSettings p2;
        CHECK(p2.Deserialize(p.Serialize()));
        CHECK(p2.defaultSampleRate == 96000);
        CHECK(p2.metronomeSound == 2);
        CHECK(std::fabs(p2.metronomeLevel - 0.75f) < 1e-3);
        CHECK(std::fabs(p2.uiScale - 1.5f) < 1e-3);
        CHECK(p2.projectDir == "/boot/home/My Projects");
        CHECK(p2.autosaveSeconds == 300);

        // A file from before the window existed keeps every default: the
        // 48 kHz new-project rate, the click sound, the 30 s autosave.
        AppSettings old;
        CHECK(old.Deserialize("buffer 256\nmetronome 1\n"));
        CHECK(old.defaultSampleRate == 48000);
        CHECK(old.metronomeSound == 0);
        CHECK(std::fabs(old.metronomeLevel - 0.30f) < 1e-3);
        CHECK(old.uiScale == 0.0f);          // follow the system font
        CHECK(old.projectDir.empty());
        CHECK(old.autosaveSeconds == 30);

        // An empty folder value is a value ("the panel's own default"), not a
        // missing line: it must survive a round trip.
        AppSettings empty;
        empty.projectDir = "";
        AppSettings empty2;
        empty2.projectDir = "/stale";
        CHECK(empty2.Deserialize(empty.Serialize()));
        CHECK(empty2.projectDir.empty());
    }

    // --- the sanitiser: a hand-edited file cannot break the app ----------
    {
        // A buffer that is not one of the offered sizes snaps to the closest;
        // a missing/garbage one (0, or a negative) comes back as the default
        // -- 0 frames would divide by zero in the audio path.
        AppSettings s;
        CHECK(s.Deserialize("buffer 500\n"));
        CHECK(s.bufferFrames == 512);
        AppSettings z;
        CHECK(z.Deserialize("buffer 0\n"));
        CHECK(z.bufferFrames == 512);
        AppSettings neg;
        CHECK(neg.Deserialize("buffer -64\n"));
        CHECK(neg.bufferFrames == 512);
        AppSettings big;
        CHECK(big.Deserialize("buffer 99999\n"));
        CHECK(big.bufferFrames == 2048);
        // ... and the offered sizes themselves are left exactly alone.
        for (int i = 0; i < kBufferFrameChoiceCount; i++) {
            AppSettings k;
            CHECK(k.Deserialize("buffer " + std::to_string(kBufferFrameChoices[i]) + "\n"));
            CHECK(k.bufferFrames == kBufferFrameChoices[i]);
        }

        // The new-project rate: garbage -> the default, an odd rate -> nearest.
        AppSettings r;
        CHECK(r.Deserialize("defrate garbage\n"));
        CHECK(r.defaultSampleRate == 48000);
        AppSettings r2;
        CHECK(r2.Deserialize("defrate 44000\n"));
        CHECK(r2.defaultSampleRate == 44100);

        // Level, sound and scale are clamped to what the engine and Themed()
        // can actually work with.
        AppSettings m;
        CHECK(m.Deserialize("metlvl 4.5\nmetsnd 9\nuiscale -2\n"));
        CHECK(std::fabs(m.metronomeLevel - 1.0f) < 1e-3);
        CHECK(m.metronomeSound == kMetronomeSoundCount - 1);
        CHECK(m.uiScale == 0.0f);
        AppSettings m2;
        CHECK(m2.Deserialize("metlvl -1\nmetsnd -3\n"));
        CHECK(m2.metronomeLevel == 0.0f);
        CHECK(m2.metronomeSound == 0);
        AppSettings m3;
        CHECK(m3.Deserialize("uiscale 9\n"));
        CHECK(m3.uiScale == 3.0f);

        // Autosave: 0 is off and stays off; a silly interval is clamped.
        AppSettings a0;
        CHECK(a0.Deserialize("autosv 0\n"));
        CHECK(a0.autosaveSeconds == 0);
        AppSettings a1;
        CHECK(a1.Deserialize("autosv 1\n"));   // too often: the shortest sane one
        CHECK(a1.autosaveSeconds == 5);
        AppSettings a2;
        CHECK(a2.Deserialize("autosv 99999\n"));
        CHECK(a2.autosaveSeconds == 3600);
        AppSettings a3;
        CHECK(a3.Deserialize("autosv -30\n"));
        CHECK(a3.autosaveSeconds == 0);

        // The old numeric guard still holds and the sanitiser does not defeat
        // it: a good line beside a bad one is still parsed.
        AppSettings e;
        CHECK(e.Deserialize("countin 3\n"));
        CHECK(e.countInBars == 3);
        AppSettings e2;
        CHECK(e2.Deserialize("countin 99\n"));
        CHECK(e2.countInBars == 8);   // clamped, not wrapped
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
