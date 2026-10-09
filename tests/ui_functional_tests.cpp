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
#include "../src/ui/PianoRoll.h"
#include "../src/ui/QuantizeWindow.h"   // kMsgRollQuantize (the roll's settings)
#include "../src/model/MidiOps.h"       // QuantGrid
#include "../src/model/Project.h"
#include "../src/model/Command.h"

#include <Application.h>
#include <Entry.h>
#include <Messenger.h>
#include <OS.h>
#include <Window.h>

#include <cstdio>
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

// A window that is not the one under test (the file panel the export dialog
// opens, or the probe), closed by the test as the panel's own Cancel would.
static void CloseOtherWindows(BWindow* keep) {
    for (int32 i = be_app->CountWindows() - 1; i >= 0; i--) {
        BWindow* w = be_app->WindowAt(i);
        if (w && w != keep) {
            w->Lock();
            w->Quit();
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

// --- 2. package 04's path, minus the mouse ---------------------------------

// A real PianoRoll on its own looper, the settings message its dialog posts,
// and the commit that lands in the model as one named undo step.
static void TestPianoRollQuantize(MainWindow* win, Project& project,
                                  CommandStack& stack) {
    std::printf("test_piano_roll_quantize\n");
    Track t = MakeMidiTrack(project, { { 60, 100, 1000, 500 },
                                       { 62, 100, 7000, 500 } }, "roll-synth");
    CHECK(project.AddTrack(t));
    const ClipId cid = t.midiClips.front().id;
    const TrackId tid = t.id;

    // What TimelineView::OpenPianoRollForClip does, without the double-click.
    PianoRoll* roll = new PianoRoll(BRect(90, 90, 810, 570), tid, cid, 0,
                                    t.midiClips.front().lengthFrames,
                                    t.midiClips.front().notes, {},
                                    project.tempoMap, project.sampleRate, -1,
                                    BMessenger(win));
    roll->Show();
    snooze(300000);
    BView* view = roll->FindView("roll");
    CHECK(view != nullptr);

    if (view) {
        // What QuantizeWindow posts: 1/16, full strength, no swing.
        BMessage q(kMsgRollQuantize);
        q.AddInt32("grid", (int32)QuantGrid::Sixteenth);
        q.AddInt32("strength", 100);
        q.AddInt32("swing", 0);
        q.AddBool("lengths", false);
        // A view is not a looper: this is what a widget's own post ends up as,
        // a message delivered to the view on the roll's thread.
        BMessenger(view).SendMessage(&q);
    }

    // 1000 -> 0 and 7000 -> 6000 on the 16th grid at the default tempo.
    CHECK(WaitFor([&] {
        if (!win->Lock()) return false;
        const Track* tr = project.FindTrack(tid);
        const MidiClip* c = tr ? tr->FindMidiClip(cid) : nullptr;
        const bool snapped = c && c->notes.size() == 2 &&
                             c->notes[0].startFrame == 0 &&
                             c->notes[1].startFrame == 6000;
        win->Unlock();
        return snapped;
    }));
    // ...and it is one undoable step, named after the transform.
    CHECK(stack.UndoName() == "Quantize");

    roll->Lock();
    roll->Quit();
    snooze(200000);
}

// --- 3. R1's export flow, minus the file panel -----------------------------

// The dialog's answer opens the panel; the panel's answer (posted here by
// hand) starts the worker; the window keeps answering while it renders; the
// file appears and the temp does not survive.
static void TestExportFlow(MainWindow* win, Project& project) {
    std::printf("test_export_flow\n");
    Track t = MakeMidiTrack(project, { { 69, 110, 0, 24000 } }, "bounce-synth");
    CHECK(project.AddTrack(t));

    const int32 windowsBefore = be_app->CountWindows();
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
    CHECK(WaitFor([&] { return be_app->CountWindows() == windowsBefore + 1; }));
    std::remove(kExportPath);
    CHECK(!FileExists(kExportPath));
    CloseOtherWindows(win);

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


// Every transform's commit path, plus the keyboard one: the menu is a popup a
// test cannot open, so RunMidiOp (what each item calls) is driven directly,
// and 'q' goes through the view's own KeyDown.
static void TestPianoRollTransforms(MainWindow* win, Project& project,
                                    CommandStack& stack) {
    std::printf("test_piano_roll_transforms\n");
    Track t = MakeMidiTrack(project, { { 60, 100, 1000, 400 },
                                       { 64, 100, 5000, 400 } }, "fx-synth");
    CHECK(project.AddTrack(t));
    const ClipId cid = t.midiClips.front().id;
    const TrackId tid = t.id;

    PianoRoll* roll = new PianoRoll(BRect(90, 90, 810, 570), tid, cid, 0,
                                    t.midiClips.front().lengthFrames,
                                    t.midiClips.front().notes, {},
                                    project.tempoMap, project.sampleRate, -1,
                                    BMessenger(win));
    roll->Show();
    snooze(300000);
    BView* view = roll->FindView("roll");
    PianoRollView* rv = dynamic_cast<PianoRollView*>(view);
    CHECK(rv != nullptr);
    if (!rv) {
        roll->Lock(); roll->Quit();
        return;
    }

    auto notes = [&](std::vector<MidiNote>* out) {
        if (!win->Lock()) return false;
        const Track* tr = project.FindTrack(tid);
        const MidiClip* c = tr ? tr->FindMidiClip(cid) : nullptr;
        if (c) *out = c->notes;
        const bool ok = c != nullptr;
        win->Unlock();
        return ok;
    };

    // 'q': the last-used quantize. A KEY_DOWN message rather than a direct
    // KeyDown() call: the handler reads the CURRENT message (auto-repeat), so
    // only a dispatched key sees what a real one would.
    {
        // The shape BView::MessageReceived expects: "bytes" as a STRING (it
        // strips the terminator itself) and "modifiers" from the message.
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", "q");
        key.AddInt32("modifiers", 0);
        BMessenger(view).SendMessage(&key);
    }
    std::vector<MidiNote> n;
    CHECK(WaitFor([&] {
        if (!notes(&n) || n.size() != 2) return false;
        return n[0].startFrame == 0 && n[1].startFrame == 6000;
    }, 3000000));
    // Diagnostics for the key path: what the model holds, and whether the view
    // would quantize at all if asked directly.
    {
        std::vector<MidiNote> d;
        notes(&d);
        std::printf("  after key: %zu notes", d.size());
        for (const MidiNote& m : d)
            std::printf(" (p%d s%lld v%d)", m.pitch, (long long)m.startFrame,
                        m.velocity);
        std::printf("  undo=%s\n", stack.UndoName().c_str());
    }
    roll->Lock();
    rv->RunMidiOp(MidiOp::Quantize);   // the path the menu item calls
    roll->Unlock();
    std::vector<MidiNote> d2;
    CHECK(WaitFor([&] {
        if (!notes(&d2) || d2.size() != 2) return false;
        return d2[0].startFrame == 0 && d2[1].startFrame == 6000;
    }));
    CHECK(stack.UndoName() == "Quantize");

    // Humanize: a fresh take each time (times/velocities move, and the model
    // says so).
    roll->Lock();
    rv->RunMidiOp(MidiOp::Humanize);
    roll->Unlock();
    std::vector<MidiNote> h;
    CHECK(WaitFor([&] {
        if (!notes(&h) || h.size() != 2) return false;
        return h[0].startFrame != 0 || h[0].velocity != 100;
    }));
    CHECK(stack.UndoName() == "Humanize");

    // Legato: the first note stretches to the second one's start.
    roll->Lock();
    rv->RunMidiOp(MidiOp::Legato);
    roll->Unlock();
    std::vector<MidiNote> l;
    CHECK(WaitFor([&] {
        if (!notes(&l) || l.size() != 2) return false;
        // It reaches exactly the NEXT note's start -- which humanize has moved,
        // so the assertion is the relation, not a literal 6000.
        return l[0].lengthFrames > 400 &&
               l[0].startFrame + l[0].lengthFrames == l[1].startFrame;
    }));

    // Transpose +12 and -12 back: the pitches move, and the name says so.
    roll->Lock();
    rv->RunMidiOp(MidiOp::Transpose, 12);
    roll->Unlock();
    std::vector<MidiNote> up;
    CHECK(WaitFor([&] {
        if (!notes(&up) || up.size() != 2) return false;
        return up[0].pitch == l[0].pitch + 12;
    }));
    CHECK(stack.UndoName() == "Transpose");

    // Velocity -10: each note drops by ten, clamped.
    roll->Lock();
    rv->RunMidiOp(MidiOp::Velocity, -10);
    roll->Unlock();
    std::vector<MidiNote> v;
    CHECK(WaitFor([&] {
        if (!notes(&v) || v.size() != 2) return false;
        return v[0].velocity == (up[0].velocity - 10 > 1 ? up[0].velocity - 10 : 1);
    }));
    CHECK(stack.UndoName() == "Adjust Velocity");

    roll->Lock();
    roll->Quit();
    snooze(200000);
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
    TestPianoRollQuantize(win, project, stack);
    TestPianoRollTransforms(win, project, stack);
    TestExportFlow(win, project);

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
