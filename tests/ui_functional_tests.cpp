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
#include "../src/model/Commands.h"   // SetFxCommand
#include "../src/engine/WavSource.h"   // reading a bounce back
#include "Version.h"                   // DAW_VERSION_STRING (generated)
#include "../src/engine/DeviceLatency.h"   // R3: what the device costs
#include "../src/model/RecordPlan.h"      // LatencyUsToFrames
#ifdef DAW_HAVE_LV2
#include "../src/plugin/Lv2Host.h"
#include "../src/ui/Lv2UiWindow.h"
#include "../src/ui/EffectsWindow.h"    // the editor messages, MakeInsertDesc
#endif

#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <MediaNode.h>
#include <MediaRoster.h>
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

// Add a track with the window locked. The test thread is the only WRITER, but
// the window looper reads the model continuously (and the export worker reads
// its snapshot); the file's own rule -- every model read outside the window
// thread takes the lock -- applies to writes too.
static bool LockedAddTrack(MainWindow* win, Project& p, const Track& t) {
    if (!win->Lock()) return false;
    const bool ok = p.AddTrack(t);
    win->Unlock();
    return ok;
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

// Wait until only the main window is up: the previous test's export bar closes
// on a pulse, and a stale bar would shift every window count that follows (and
// make a later "the bar appeared" assertion pass vacuously).
static bool WaitQuiet(bigtime_t timeoutUs = 30000000) {
    return WaitFor([&] { return VisibleWindows() == 1; }, timeoutUs);
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

// --- 2. package 04's path, minus the mouse ---------------------------------

// A real PianoRoll on its own looper, the settings message its dialog posts,
// and the commit that lands in the model as one named undo step.
static void TestPianoRollQuantize(MainWindow* win, Project& project,
                                  CommandStack& stack) {
    std::printf("test_piano_roll_quantize\n");
    Track t = MakeMidiTrack(project, { { 60, 100, 1000, 500 },
                                       { 62, 100, 7000, 500 } }, "roll-synth");
    CHECK(LockedAddTrack(win, project, t));
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
    CHECK(WaitQuiet());
    std::printf("test_export_flow\n");
    Track t = MakeMidiTrack(project, { { 69, 110, 0, 24000 } }, "bounce-synth");
    CHECK(LockedAddTrack(win, project, t));

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
    // (Under the lock: the looper is handling the export messages meanwhile.)
    size_t before = 0;
    if (win->Lock()) { before = project.Tracks().size(); win->Unlock(); }
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
    CHECK(WaitQuiet());   // its bar closes on a pulse
}

// --- 4. R1: cancel, stems, loop range --------------------------------------

// Cancel is a flag the worker polls: the run stops, and NOTHING is left where
// a finished file is expected.
static void TestExportCancel(MainWindow* win, Project& project) {
    CHECK(WaitQuiet());
    std::printf("test_export_cancel\n");
    const char* path = "/tmp/haiku_daw_ui_cancel.wav";
    std::remove(path);
    // Long enough that the render is still going when the cancel lands.
    Track t = MakeMidiTrack(project, { { 60, 100, 0, 48000 * 8 } }, "long-synth");
    CHECK(LockedAddTrack(win, project, t));

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
    CHECK(WaitQuiet());
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
    CHECK(WaitQuiet());
    std::printf("test_export_loop_range\n");
    const char* path = "/tmp/haiku_daw_ui_loop.wav";
    std::remove(path);

    // A loop over the second second of the project (under the lock: the
    // looper reads the transport continuously).
    if (win->Lock()) {
        project.transport.loopEnabled = true;
        project.transport.loopStart = 48000;
        project.transport.loopEnd   = 96000;
        win->Unlock();
    }

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
    CHECK(WaitQuiet());
    std::remove(path);
    if (win->Lock()) { project.transport.loopEnabled = false; win->Unlock(); }
}


// Every transform's commit path, plus the keyboard one: the menu is a popup a
// test cannot open, so RunMidiOp (what each item calls) is driven directly,
// and 'q' goes through the view's own KeyDown.
static void TestPianoRollTransforms(MainWindow* win, Project& project,
                                    CommandStack& stack) {
    std::printf("test_piano_roll_transforms\n");
    // 5700, not 5000: the 16th-grid target is 6000, and the region has to
    // cover it, or the window rule clamps the note to the region's last frame
    // (correct behaviour, bad fixture).
    Track t = MakeMidiTrack(project, { { 60, 100, 1000, 400 },
                                       { 64, 100, 5700, 400 } }, "fx-synth");
    CHECK(LockedAddTrack(win, project, t));
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
    }));
    CHECK(stack.UndoName() == "Quantize");

    // Humanize: a fresh take each time (times/velocities move, and the model
    // says so).
    roll->Lock();
    rv->RunMidiOp(MidiOp::Humanize);
    roll->Unlock();
    std::vector<MidiNote> before;
    CHECK(notes(&before));
    std::vector<MidiNote> h;
    // Against the state before it, not against one note: humanize draws for
    // each note, and a clamped timing draw plus a zero velocity draw can leave
    // any single note exactly as it was.
    CHECK(WaitFor([&] {
        if (!notes(&h) || h.size() != before.size()) return false;
        return !NotesEqual(h, before);
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

// --- 5. R3: the device latency the recorder compensates with ---------------

// The helper sums the output and input nodes' latencies and converts to
// frames; the test asks the roster ITSELF and checks the arithmetic against
// it, so a helper that queried the wrong node, dropped a term or returned
// nonsense fails here -- a take cannot be recorded in a test, but the number
// that places it can be.
static void TestDeviceLatency() {
    std::printf("test_device_latency\n");
    const Frame frames = DeviceRoundTripFrames(48000.0);
    CHECK(frames >= 0);

    BMediaRoster* roster = BMediaRoster::Roster();
    CHECK(roster != nullptr);
    if (roster) {
        media_node out, in;
        bigtime_t lo = 0, li = 0;
        const bool have = roster->GetAudioOutput(&out) == B_OK
                       && roster->GetAudioInput(&in) == B_OK
                       && roster->GetLatencyFor(out, &lo) == B_OK
                       && roster->GetLatencyFor(in, &li) == B_OK;
        std::printf("  device round trip: %lld frames (%s)\n",
                    (long long)frames,
                    have ? "the roster answered" : "no device to ask");
        if (have)
            CHECK(frames == LatencyUsToFrames((int64_t)(lo + li), 48000.0));
    }
}

// --- 6. R4: the About box knows the version --------------------------------

// Help > About opens a window, and it is the app's own version (the generated
// header), not a hand-typed string that can drift from CMakeLists.
static void TestAboutBox(MainWindow* win) {
    std::printf("test_about_box\n");
    CHECK(WaitQuiet());
    const int32 before = VisibleWindows();
    win->PostMessage(MSG_ABOUT);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
    HideOtherWindows(win);   // the alert, as its OK button does
    std::printf("  version: %s\n", DAW_VERSION_STRING);
    CHECK(std::strlen(DAW_VERSION_STRING) > 0);
    CHECK(std::strcmp(DAW_VERSION_STRING, "1.0.0") == 0);
}


#ifdef DAW_HAVE_LV2
// --- 7. package 07: the host half of a native editor ------------------------

// What the wiring between a plugin's own editor and the model does, without
// the editor's GUI: MainWindow opens one for an insert, its debounced commit
// lands as one named undo step, the save/export flush is answered, and a chain
// edit closes the editor. The plugin's window itself still needs a person --
// it renders in the plugin's own code -- but everything on our side of it is
// here.
static void TestLv2EditorWiring(MainWindow* win, Project& project,
                                CommandStack& stack) {
    std::printf("test_lv2_editor_wiring\n");
    // main() does this at startup; a test binary is its own application.
    Lv2Host::Instance().ScanAll();
    const std::vector<Lv2PluginInfo>& plugins = Lv2Host::Instance().Plugins();
    const Lv2PluginInfo* chosen = nullptr;
    for (const Lv2PluginInfo& p : plugins)
        if (Lv2UiWindow::HasNativeUi(p.uri) && !p.params.empty()) {
            chosen = &p;
            break;
        }
    if (!chosen) {
        std::printf("  no plugin with a native UI installed - nothing to drive\n");
        return;                       // the machine may have none; not a failure
    }
    std::printf("  driving %s\n", chosen->uri.c_str());

    Track t = MakeMidiTrack(project, { { 60, 100, 0, 4800 } }, "lv2-synth");
    t.fx.push_back(MakeInsertDesc(EffectType::Lv2, chosen->uri));
    const size_t fxIndex = t.fx.size() - 1;
    const TrackId tid = t.id;
    CHECK(LockedAddTrack(win, project, t));

    // Open it the way the inspector's slot list does.
    const int32 before = VisibleWindows();
    BMessage open(kMsgOpenFxEditor);
    open.AddInt64("track", (int64)tid);
    open.AddInt32("fx", (int32)fxIndex);
    win->PostMessage(&open);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; },
                  30000000));

    // The committed write an editor posts when a gesture goes quiet: one undo
    // step, in the model.
    const float target = chosen->params[0].def + 1.0f;
    BMessage commit(kMsgFxParamCommit);
    commit.AddInt64("track", (int64)tid);
    commit.AddInt32("fx", (int32)fxIndex);
    commit.AddString("uri", chosen->uri.c_str());
    commit.AddInt32("slot", 0);
    commit.AddFloat("val", target);
    win->PostMessage(&commit);
    CHECK(WaitFor([&] {
        if (!win->Lock()) return false;
        const Track* tr = project.FindTrack(tid);
        const bool landed = tr && !tr->fx.empty() &&
                            tr->fx[fxIndex].p(0) == target;
        win->Unlock();
        return landed;
    }));
    CHECK(stack.UndoName() == "Edit Effect Parameter");

    // ...and a commit for a plugin that is not at that address any more is
    // dropped rather than written into whatever is (the identity rule).
    BMessage stale(kMsgFxParamCommit);
    stale.AddInt64("track", (int64)tid);
    stale.AddInt32("fx", (int32)fxIndex);
    stale.AddString("uri", "http://example.invalid/not-this-plugin");
    stale.AddInt32("slot", 0);
    stale.AddFloat("val", target + 5.0f);
    win->PostMessage(&stale);
    snooze(300000);
    {
        bool unchanged = false;
        if (win->Lock()) {
            const Track* tr = project.FindTrack(tid);
            unchanged = tr && !tr->fx.empty() && tr->fx[fxIndex].p(0) == target;
            win->Unlock();
        }
        CHECK(unchanged);
    }

    // The save/export flush: the editor is asked for what it has not committed
    // and answers. (The window is the one we just opened; a flush that cannot
    // be answered is the failure this guards.)
    BWindow* editor = nullptr;
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && w != win && !w->IsHidden() && w->Name() &&
            std::strstr(w->Name(), chosen->name.c_str()))
            editor = w;
    }
    CHECK(editor != nullptr);
    if (editor) {
        BMessage flush(kMsgLv2UiFlush);
        BMessage reply;
        const status_t sent = BMessenger(editor).SendMessage(&flush, &reply,
                                                             200000, 200000);
        CHECK(sent == B_OK);
    }

    // Remove the insert, posted the way the parameter panel posts a chain edit
    // (kMsgApplyFx -- NOT by executing a command here): that handler is what
    // re-checks the watch table, and it is the thing under test. The editor
    // must close rather than keep driving whatever took its index.
    {
        std::vector<EffectDesc> chain;
        if (win->Lock()) {
            const Track* tr = project.FindTrack(tid);
            if (tr) chain = tr->fx;
            win->Unlock();
        }
        chain.erase(chain.begin() + (long)fxIndex);
        BMessage edit(kMsgApplyFx);
        edit.AddInt64("track", (int64)tid);
        EncodeFxChain(edit, chain);
        win->PostMessage(&edit);
    }
    CHECK(WaitFor([&] { return VisibleWindows() == before; }, 30000000));

    // A chain that holds the plugin TWICE: its own editor cannot tell the
    // copies apart, so MainWindow must not leave the click dead -- with
    // "fallback" set (what the inspector posts) the generic panel opens
    // instead. Without the flag (the panel's own "open the plugin's editor"
    // sender) nothing new may appear: it is already a panel.
    {
        Track dup = MakeMidiTrack(project, { { 60, 100, 0, 4800 } }, "lv2-dup");
        dup.fx.push_back(MakeInsertDesc(EffectType::Lv2, chosen->uri));
        dup.fx.push_back(MakeInsertDesc(EffectType::Lv2, chosen->uri));
        CHECK(LockedAddTrack(win, project, dup));

        BMessage open(kMsgOpenFxEditor);
        open.AddInt64("track", (int64)dup.id);
        open.AddInt32("fx", 0);
        open.AddBool("fallback", true);
        win->PostMessage(&open);
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; },
                      30000000));
        HideOtherWindows(win);
        CHECK(WaitQuiet());

        BMessage again(kMsgOpenFxEditor);   // no fallback: the panel's sender
        again.AddInt64("track", (int64)dup.id);
        again.AddInt32("fx", 0);
        win->PostMessage(&again);
        snooze(300000);
        CHECK(VisibleWindows() == 1);
    }
}
#endif

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
    TestDeviceLatency();
    TestExportCancel(win, project);
    TestExportStems(win, project);
    TestExportLoopRange(win, project);
    TestAboutBox(win);
#ifdef DAW_HAVE_LV2
    TestLv2EditorWiring(win, project, stack);
#endif

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
