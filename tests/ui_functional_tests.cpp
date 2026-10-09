// Functional tests for the UI, on the target: a real BApplication, the REAL
// windows, and messages posted exactly the way the widgets post them.
//
// This is the automated half of what used to be a hand-written click list. It
// needs a reachable app_server -- a process launched over SSH does reach this
// VM's server, which is what made the approach possible -- and reports a SKIP
// (exit 77) where there is none, so "no display" is never counted as a pass.
//
// The test body runs on its own thread while BApplication::Run() drives the
// message loop (a window only receives messages while its looper runs). The
// model is read under a window's lock, the way any other looper-external reader
// has to.
#include "../src/ui/MainWindow.h"
#include "../src/model/Project.h"
#include "../src/model/Command.h"
#include "../src/engine/WavSource.h"   // reading a bounce back

#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Window.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kExportName = "haiku_daw_ui_export.wav";
static const char* kExportPath = "/tmp/haiku_daw_ui_export.wav";

// Poll `pred` for up to `timeoutUs` (pred runs on this thread; it takes the
// window lock itself when it reads the model).
static bool WaitFor(const std::function<bool()>& pred,
                    bigtime_t timeoutUs = 15000000) {
    const bigtime_t step = 20000;
    bigtime_t waited = 0;
    for (;;) {
        if (pred()) return true;
        if (waited >= timeoutUs) return false;
        snooze(step);
        waited += step;
    }
}

// How many .wav files a directory holds (0 when it does not exist yet).
static int CountWavs(const std::string& dir) {
    BDirectory d(dir.c_str());
    if (d.InitCheck() != B_OK) return 0;
    int n = 0;
    BEntry e;
    d.Rewind();
    while (d.GetNextEntry(&e) == B_OK) {
        BPath p;
        if (e.GetPath(&p) != B_OK) continue;
        const size_t len = std::strlen(p.Path());
        if (len > 4 && std::strcmp(p.Path() + len - 4, ".wav") == 0) n++;
    }
    return n;
}

static bool FileExists(const std::string& p) {
    BEntry e(p.c_str());
    return e.Exists();
}

// A MIDI track whose region starts at 0 and covers the notes it is given.
static Track MakeMidiTrack(Project& p, const std::vector<MidiNote>& notes,
                           const char* name) {
    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Midi;
    t.name = name;
    t.gain = 1.0f;
    MidiClip c;
    c.id = p.NextClipId();
    c.startFrame = 0;
    Frame end = 1;
    for (const MidiNote& n : notes) {
        c.notes.push_back(n);
        if (n.startFrame + n.lengthFrames > end)
            end = n.startFrame + n.lengthFrames;
    }
    c.lengthFrames = end;
    t.midiClips.push_back(c);
    return t;
}

// Visible windows: what "a window appeared" means to a user, and what the
// tests assert on. (CountWindows() includes hidden ones, and MainWindow KEEPS
// its file panels alive and hidden for the next time -- quitting one behind its
// back would leave the window holding a dangling pointer.)
static int VisibleWindows() {
    int n = 0;
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && !w->IsHidden()) n++;
    }
    return n;
}

// Put a window that is not the one under test away -- the panel, as its own
// Cancel button does: HIDE it, never Quit it.
static void HideOtherWindows(BWindow* keep) {
    for (int32 i = be_app->CountWindows() - 1; i >= 0; i--) {
        BWindow* w = be_app->WindowAt(i);
        if (w && w != keep && !w->IsHidden()) {
            w->Lock();
            w->Hide();
            w->Unlock();
        }
    }
    snooze(200000);
}

// --- 1. the harness itself -------------------------------------------------

// A real MainWindow exists and a posted message is handled on its looper: the
// model gains a track. Everything else here rests on this round trip.
static void TestMessageRoundTrip(MainWindow* win, Project& project) {
    std::printf("test_message_round_trip\n");
    const size_t before = project.Tracks().size();
    win->PostMessage(MSG_NEW_MIDI);
    CHECK(WaitFor([&] {
        if (!win->Lock()) return false;
        const size_t now = project.Tracks().size();
        win->Unlock();
        return now == before + 1;
    }));
}

// --- 2. package 04's path lives on its own branch ---------------------------
//
// The piano roll's transform path (a real PianoRoll, kMsgRollQuantize, and the
// named undo step it leaves) is written and waiting where that package is
// merged: it needs kMsgRollQuantize and QuantGrid, neither of which is in
// master until package 04 lands. Its body is in this file's history on
// feature/ui-functional-tests-04; it moves here in the merge.

// --- 3. R1's export flow, minus the file panel -----------------------------

// The dialog's answer opens the panel; the panel's answer (posted here by
// hand) starts the worker; the window keeps answering while it renders; the
// file appears and the temp does not survive.
static void TestExportFlow(MainWindow* win, Project& project) {
    std::printf("test_export_flow\n");
    Track t = MakeMidiTrack(project, { { 69, 110, 0, 24000 } }, "bounce-synth");
    CHECK(project.AddTrack(t));

    const int32 windowsBefore = VisibleWindows();
    BMessage opts(kMsgExportOptions);
    opts.AddInt32("bits", 16);
    opts.AddBool("dither", true);
    opts.AddInt32("rate", 0);
    opts.AddBool("norm", false);
    opts.AddFloat("lufs", -14.0f);
    opts.AddFloat("ceil", -1.0f);
    opts.AddBool("lim", false);
    opts.AddInt32("range", 0);
    opts.AddInt32("stems", 0);
    win->PostMessage(&opts);

    // Exactly one window appears (the file panel) and nothing renders yet.
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore + 1; }));
    std::remove(kExportPath);
    CHECK(!FileExists(kExportPath));
    HideOtherWindows(win);

    // What that panel posts when a name is chosen.
    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage ref(MSG_EXPORT_REF);
    ref.AddRef("directory", &dir);
    ref.AddString("name", kExportName);
    win->PostMessage(&ref);

    // The window must keep answering while it renders: this track has to land.
    const size_t before = project.Tracks().size();
    win->PostMessage(MSG_NEW_MIDI);
    CHECK(WaitFor([&] {
        if (!win->Lock()) return false;
        const size_t now = project.Tracks().size();
        win->Unlock();
        return now == before + 1;
    }));

    CHECK(WaitFor([&] { return FileExists(kExportPath); }, 60000000));
    CHECK(FileExists(kExportPath));
    CHECK(!FileExists(std::string(kExportPath) + ".part"));
}

// --- 4. R1: cancel, stems, loop range --------------------------------------

// Cancel is a flag the worker polls: the run stops, and NOTHING is left where
// a finished file is expected.
static void TestExportCancel(MainWindow* win, Project& project) {
    std::printf("test_export_cancel\n");
    const char* path = "/tmp/haiku_daw_ui_cancel.wav";
    std::remove(path);
    // Long enough that the render is still going when the cancel lands.
    Track t = MakeMidiTrack(project, { { 60, 100, 0, 48000 * 8 } }, "long-synth");
    CHECK(project.AddTrack(t));

    const int32 windowsBefore = VisibleWindows();
    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage ref(MSG_EXPORT_REF);
    ref.AddRef("directory", &dir);
    ref.AddString("name", "haiku_daw_ui_cancel.wav");
    win->PostMessage(&ref);

    // It is running (its bar is up) when the cancel arrives.
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore + 1; }));
    win->PostMessage(kMsgExportCancel);

    // The bar goes away, and the destination is left alone.
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore; },
                  60000000));
    snooze(300000);
    CHECK(!FileExists(path));
    CHECK(!FileExists(std::string(path) + ".part"));
}

// Stems: the dialog's answer opens the folder panel, and its answer writes one
// file per non-bus track.
static void TestExportStems(MainWindow* win, Project& project) {
    std::printf("test_export_stems\n");
    const std::string dir = "/tmp/haiku_daw_ui_stems";
    const std::string rm = "rm -rf " + dir;
    if (system(rm.c_str()) != 0) return;

    int expected = 0;
    for (const Track& tr : project.Tracks()) {
        if (tr.type == TrackType::Bus) continue;
        if (tr.clips.empty() && tr.midiClips.empty()) continue;   // nothing to render
        expected++;
    }
    CHECK(expected > 0);

    const int32 windowsBefore = VisibleWindows();
    BMessage opts(kMsgExportOptions);
    opts.AddInt32("bits", 16);
    opts.AddBool("dither", true);
    opts.AddInt32("rate", 0);
    opts.AddBool("norm", false);
    opts.AddFloat("lufs", -14.0f);
    opts.AddFloat("ceil", -1.0f);
    opts.AddBool("lim", false);
    opts.AddInt32("range", 0);
    opts.AddInt32("stems", 1);          // the dialog's "separate stems" box
    win->PostMessage(&opts);
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore + 1; }));
    HideOtherWindows(win);

    entry_ref base;
    CHECK(BEntry("/tmp").GetRef(&base) == B_OK);
    BMessage ref(MSG_EXPORT_STEMS_REF);
    ref.AddRef("directory", &base);
    ref.AddString("name", "haiku_daw_ui_stems");
    win->PostMessage(&ref);

    // One file per track, whatever they are called (the names carry the track
    // and its number, and depending on the order the tests ran in, both move).
    CHECK(WaitFor([&] { return CountWavs(dir) == expected; }, 60000000));

    if (system(rm.c_str()) != 0) return;
}

// The loop range bounces what the loop covers, not the whole timeline.
static void TestExportLoopRange(MainWindow* win, Project& project) {
    std::printf("test_export_loop_range\n");
    const char* path = "/tmp/haiku_daw_ui_loop.wav";
    std::remove(path);

    // A loop over the second second of the project.
    project.transport.loopEnabled = true;
    project.transport.loopStart = 48000;
    project.transport.loopEnd   = 96000;

    const int32 windowsBefore = VisibleWindows();
    BMessage opts(kMsgExportOptions);
    opts.AddInt32("bits", 32);          // float: the length is what is read
    opts.AddBool("dither", false);
    opts.AddInt32("rate", 0);
    opts.AddBool("norm", false);
    opts.AddFloat("lufs", -14.0f);
    opts.AddFloat("ceil", -1.0f);
    opts.AddBool("lim", false);
    opts.AddInt32("range", 1);          // loop range
    opts.AddInt32("stems", 0);
    win->PostMessage(&opts);
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore + 1; }));
    HideOtherWindows(win);

    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage ref(MSG_EXPORT_REF);
    ref.AddRef("directory", &dir);
    ref.AddString("name", "haiku_daw_ui_loop.wav");
    win->PostMessage(&ref);

    CHECK(WaitFor([&] { return FileExists(path); }, 60000000));
    WavSource src;
    CHECK(src.Open(path));
    CHECK(src.TotalFrames() > 0);
    // One second of loop, not the whole project (a frame of slack).
    CHECK(src.TotalFrames() >= 48000 - 2 && src.TotalFrames() <= 48000 + 2);
    std::remove(path);
    project.transport.loopEnabled = false;
}

// --- driver ----------------------------------------------------------------

static int32 TestThread(void*) {
    // One project for the whole run: windows come and go, the model stays, and
    // nothing is freed while a looper could still be reading it.
    Project project;
    project.sampleRate = 48000.0;
    CommandStack stack;
    MainWindow::PeakMap peaks;

    // ONE window for the run: closing the main window quits the app (that is
    // what QuitRequested does), so a per-test window would end the run early.
    MainWindow* win = new MainWindow(BRect(60, 60, 900, 660), &project, &stack,
                                     &peaks);
    win->Show();
    snooze(300000);

    TestMessageRoundTrip(win, project);
    TestExportFlow(win, project);
    TestExportCancel(win, project);
    TestExportStems(win, project);
    TestExportLoopRange(win, project);

    std::printf("\nui_functional_tests: %d checks, %d failures\n", g_checks,
                g_fails);
    std::fflush(stdout);
    win->Lock();
    win->Quit();   // and with it the application
    return 0;
}

int main() {
    BApplication app("application/x-haiku-daw-uitests");
    if (app.InitCheck() != B_OK) {
        std::printf("ui_functional_tests: no app_server - skipping\n");
        return 77;   // CTest SKIP_RETURN_CODE
    }
    // A window is the only honest proof that there is a display to drive.
    BWindow* probe = new BWindow(BRect(0, 0, 40, 40), "probe",
                                 B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL,
                                 B_AVOID_FOCUS);
    if (!probe->Lock()) {
        std::printf("ui_functional_tests: no display - skipping\n");
        return 77;
    }
    probe->Show();
    probe->Unlock();
    snooze(200000);
    probe->Lock();
    probe->Quit();

    thread_id th = spawn_thread(TestThread, "tests", B_NORMAL_PRIORITY, nullptr);
    resume_thread(th);
    app.Run();
    return g_fails == 0 ? 0 : 1;
}
