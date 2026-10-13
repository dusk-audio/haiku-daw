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
#include "../src/ui/MeterView.h"      // the master meter (M3.3)
#include "../src/ui/ShortcutsWindow.h" // kShortcutsTextView + kMsgShortcutsActivate
#include "../src/model/KeyMap.h"      // the one keyboard map (M3.2)
#include "../src/ui/ProjectDocument.h"   // SettingsPath (the recovery file)
#include "../src/ui/TimelineView.h"   // the focus check casts CurrentFocus()
#include "../src/ui/PianoRoll.h"
#include "../src/ui/QuantizeWindow.h"   // kMsgRollQuantize (the roll's settings)
#include "../src/ui/RenameWindow.h"     // kMsgRenameClip (the Edit > Rename flow)
#include "../src/ui/MixerWindow.h"      // MixerStripsView::Strips (the mid-drag check)
#include "../src/ui/MarkersWindow.h"    // MarkersView::Markers (the M2.7 list)
#include "../src/ui/PreferencesWindow.h"   // the Preferences window (M3.5)
#include "../src/ui/widgets/DawButton.h"   // the kit (M1.3)
#include "../src/ui/widgets/DawControlLook.h"
#include "../src/ui/widgets/DawCheckBox.h"
#include "../src/ui/widgets/DawSegments.h"  // the dock's page switch (T2)
#include "../src/ui/widgets/DawStringItem.h"   // the browsers' rows (T2 review)
#include "../src/ui/widgets/DawKnob.h"
#include "../src/ui/widgets/DawSlider.h"
#include "../src/model/MidiOps.h"       // QuantGrid
#include "../src/model/RollEdit.h"   // the roll's kit-free rules (package 51)
#include "../src/model/MidiExpression.h"  // the lane's path into a voice
#include "../src/synth/Synth.h"           // ...rendered, to prove it arrives
#include "../src/model/Project.h"
#include "../src/model/ProjectIO.h"   // the Open flow's fixture file
#include "../src/model/Command.h"
#include "../src/model/Commands.h"   // SetFxCommand
#include "../src/engine/WavSource.h"   // reading a bounce back
#include "../src/engine/WavWriter.h"   // ...and writing the Locate… fixture
#include "../src/engine/AudioFormats.h"   // the format menus + sniffing a bounce
#include "Version.h"                   // DAW_VERSION_STRING (generated)
#include "../src/engine/DeviceLatency.h"   // R3: what the device costs
#include "../src/model/RecordPlan.h"      // LatencyUsToFrames
// Unconditional, though the LV2 test below is the reason the editor's header
// was pulled in first: EffectsWindow.h has its own LV2 #ifdefs, and the
// sidechain picker's test needs it (and MixerWindow's kMsgMixFx) in the
// -DDAW_LV2=OFF build too.
#include "../src/ui/EffectsWindow.h"    // the editor messages, MakeInsertDesc
#include "../src/ui/MixerWindow.h"      // kMsgMixFx (open the effects editor)
#include "../src/ui/TransportBar.h"     // TransportButton + ButtonRect (M3.1)
#include "../src/dsp/SidechainKey.h"    // kExtKeySlot (the picker's appended slot)
#ifdef DAW_HAVE_LV2
#include "../src/plugin/Lv2Host.h"
#include "../src/ui/Lv2UiWindow.h"
#endif

#include <Alert.h>
#include <Application.h>
#include <TextView.h>   // the shortcuts window's text, read back
#include <Button.h>
#include <MenuField.h>   // the export dialog's Format / Vorbis quality menus
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Directory.h>
#include <FindDirectory.h>   // find_path (the template directory, M3.7)
#include <kernel/image.h>    // B_APP_IMAGE_SYMBOL
#include <Entry.h>
#include <File.h>
#include <ListView.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <StringItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <MediaNode.h>
#include <MediaRoster.h>
#include <Window.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// MainWindow's private message ids the tests post (the enum is file-local
// there, so a test that drives them states them here).
#ifndef MSG_POP_OUT_EDITOR
#define MSG_POP_OUT_EDITOR 'poed'
#endif
#ifndef MSG_TOGGLE_INSPECTOR
#define MSG_TOGGLE_INSPECTOR 'tins'
#endif
#ifndef MSG_SHORTCUTS
#define MSG_SHORTCUTS 'keys'
#endif
// The transport ids, for a test that needs the transport without the focus
// rules the key path has (TestKeyMap drives the keys themselves).
#define MSG_PLAY 'play'
#define MSG_STOP 'stop'
#ifndef MSG_TOGGLE_DOCK
#define MSG_TOGGLE_DOCK 'tdck'
#endif
#ifndef kMsgMixFxAdd
#define kMsgMixFxAdd 'mxfa'   // the channel strip's "add an effect" (T2: docked)
#endif
#ifndef MSG_UNDO_TEST
#define MSG_UNDO_TEST 'undo'  // Edit > Undo (MainWindow's MSG_UNDO)
#endif
#ifndef MSG_FOLLOW_TEST
#define MSG_FOLLOW_TEST 'folw'   // View > Follow Playhead (package 51)
#endif
#ifndef MSG_AUDITION_IDLE_TEST
#define MSG_AUDITION_IDLE_TEST 'auid'   // the audition's idle stopper (package 51)
#endif
#ifndef MSG_MIXER_TEST
#define MSG_MIXER_TEST 'mixr'   // View > Mixer (MainWindow's MSG_MIXER)
#endif
#ifndef kMsgMixSelect
#define kMsgMixSelect 'mxse'  // a mixer strip's track click (MixerWindow.h)
#endif

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

// The file-name part of a path. Media paths are canonicalised by the loader
// (/tmp is a symlink to /boot/system/cache/tmp on Haiku) and stored relative to
// the project, so an assertion that cares WHICH file a clip points at compares
// base names, not the spelling of the path.
static std::string PathBaseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The sample browser's fixture: a WAV with a name nothing else shares, in the
// app's own settings directory -- the one directory these tests may write to,
// and on the boot volume the browser's own BQuery searches.
static std::string BrowseFixturePath() {
    BPath p;
    if (!ProjectDocument::SettingsPath(p)) return std::string();
    BPath dir;
    if (p.GetParent(&dir) != B_OK) return std::string();
    return std::string(dir.Path()) + "/haiku-daw-ui-browse.wav";
}

static bool WriteSilentWav(const std::string& path) {
    WavWriter w;
    if (!w.Open(path.c_str(), 48000, 1)) return false;
    std::vector<int16_t> frames(4800, 0);   // 100 ms of silence
    const bool ok = w.WriteInt16(frames.data(), frames.size());
    return w.Close() && ok;
}

// Clips the project holds over every track (for "the import landed"): read
// under the window's lock by the caller.
static size_t CountClips(const Project& p) {
    size_t n = 0;
    for (const Track& t : p.Tracks()) n += t.clips.size();
    return n;
}

static bool ProjectHasClip(const Project& p, const std::string& baseName) {
    for (const Track& t : p.Tracks())
        for (const Clip& c : t.clips)
            if (PathBaseName(c.sourcePath) == baseName) return true;
    return false;
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
    if (win->LockWithTimeout(1000000) != B_OK) return false;
    const bool ok = p.AddTrack(t);
    win->Unlock();
    // What the app's own edit paths post after a model change: without it the
    // lanes only repaint where the playhead sweeps, and the screen lies.
    win->PostMessage(kMsgUiRefresh);
    return ok;
}

// A screenshot of the whole screen, for reviewing what each test leaves up.
// Only with DAW_UI_SHOTS=<dir> set (a no-op otherwise, so a normal run's
// timing is untouched): the pause lets the windows finish drawing, and the
// files are numbered in run order so a directory listing reads as the run.
static void Shot(const char* name) {
    static const char* dir = std::getenv("DAW_UI_SHOTS");
    static int n = 0;
    if (dir == nullptr || dir[0] == '\0') return;
    snooze(600000);
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "screenshot -s '%s/%02d-%s.png'",
                  dir, n++, name);
    if (std::system(cmd) != 0) std::printf("  shot failed: %s\n", name);
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

// The window's title, as SetTitle set it. BWindow::Name() returns the
// window's THREAD name, which Haiku prefixes with "w>" (BWindow::_SetName
// renames the thread "w>window title"), so a raw Name() comparison against a
// title never matches -- which is how the first cut of this test failed to
// find its own prompt.
static const char* WindowTitle(BWindow* w) {
    const char* name = w ? w->Name() : nullptr;
    if (!name) return "";
    return std::strncmp(name, "w>", 2) == 0 ? name + 2 : name;
}

// Is a visible window with this title up? Deliberately takes NO lock: a modal
// prompt holds the main window's looper until it is answered, so a predicate
// that locked the window first would block forever instead of failing.
static bool WindowUp(const char* title) {
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && !w->IsHidden() && std::strcmp(WindowTitle(w), title) == 0)
            return true;
    }
    return false;
}

// The one VISIBLE window with this title (nullptr when there is none), and how
// many there are -- "asking again activates the open one instead of stacking a
// second" is checked with the count. Neither takes a lock: a modal prompt holds
// the main window's looper, so a locking predicate would block instead of
// failing.
static BWindow* FindVisibleWindow(const char* title) {
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && !w->IsHidden() && std::strcmp(WindowTitle(w), title) == 0)
            return w;
    }
    return nullptr;
}

static int CountVisibleWindows(const char* title) {
    int n = 0;
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && !w->IsHidden() && std::strcmp(WindowTitle(w), title) == 0)
            n++;
    }
    return n;
}

// Is a window with this title still in the application's list AT ALL --
// hidden ones included? A window that is quitting stops being drawn before it
// leaves the list, so WindowUp() says "gone" while the object is still there.
// Waiting for THIS to go false is what makes it safe to touch afterwards.
static bool WindowListed(const char* title) {
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w && std::strcmp(WindowTitle(w), title) == 0) return true;
    }
    return false;
}

// ...and by IDENTITY, for when two windows share a title. Comparing pointers
// is safe after a Quit: the window is deleted, but comparing its address with
// the list's never dereferences it.
static bool WindowListed(BWindow* which) {
    for (int32 i = 0; i < be_app->CountWindows(); i++)
        if (be_app->WindowAt(i) == which) return true;
    return false;
}

// Wait until only the main window is up: the previous test's export bar closes
// on a pulse, and a stale bar would shift every window count that follows (and
// make a later "the bar appeared" assertion pass vacuously).
static bool WaitQuiet(bigtime_t timeoutUs = 30000000) {
    return WaitFor([&] { return VisibleWindows() == 1; }, timeoutUs);
}

// --- the UX shell (fix/ux-shell): the status line and the dialog vocabulary --
//
// Shared by the tests below and by the dialog tests further down, so they live
// with the other helpers.

// The status line as the transport bar last set it.
static std::string TransportStatus(MainWindow* win) {
    std::string s;
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (auto* bar = dynamic_cast<TransportBar*>(win->TransportBarView()))
            s = bar->StatusText();
        win->Unlock();
    }
    return s;
}

// The vocabulary the shell settled on, checked on a real window of the app's
// own class: a Cancel button and a primary one whose label is the verb; the
// primary button is the window's DEFAULT (what BWindow routes Return to, the
// registration no kit button ever made) and is focusable by Tab; and the window
// closes on Escape (B_CLOSE_ON_ESCAPE turns the key into B_QUIT_REQUESTED).
// The caller closes the window by posting exactly that.
static void CheckVocabulary(BWindow* w, const char* title,
                            const char* primaryName, const char* primaryLabel) {
    if (w == nullptr) { CHECK(w != nullptr); return; }
    if (w->LockWithTimeout(2000000) != B_OK) {
        CHECK(false);   // "the dialog answered its lock"
        return;
    }
    CHECK(std::strcmp(WindowTitle(w), title) == 0);
    CHECK((w->Flags() & B_CLOSE_ON_ESCAPE) != 0);
    DawButton* cancel = dynamic_cast<DawButton*>(w->FindView("cx"));
    DawButton* primary = dynamic_cast<DawButton*>(w->FindView(primaryName));
    CHECK(cancel != nullptr);
    CHECK(cancel != nullptr && cancel->Label() != nullptr
          && std::strcmp(cancel->Label(), "Cancel") == 0);
    CHECK(primary != nullptr);
    CHECK(primary != nullptr && primary->Label() != nullptr
          && std::strcmp(primary->Label(), primaryLabel) == 0);
    // The default registration: BWindow::_DetermineTarget routes B_ENTER to
    // the button DefaultButton() names, and BButton::KeyDown invokes on it.
    CHECK(primary != nullptr && w->DefaultButton() == primary);
    // ... which is only type-safe because a kit button IS a BButton.
    CHECK(dynamic_cast<BButton*>(primary) != nullptr);
    CHECK(primary != nullptr && (primary->Flags() & B_NAVIGABLE) != 0);
    w->Unlock();
}

static bool AlertUp(const char* title) {
    return WindowUp(title);
}

// Answer a modal alert the way its own buttons do: wait for the alert titled
// `title` to come up (it opens on the window's thread a moment after the
// message that triggers it), then take the button at `which` (0/1/2 = the
// order they were given to the constructor) and invoke it. BAlert::ButtonAt
// is the public way in; the invoke is dispatched to the alert's own looper,
// which is what releases the synchronous Go() on the window's thread.
static bool AnswerAlertWhenUp(const char* title, int32 which,
                              bigtime_t timeoutUs = 10000000) {
    const bigtime_t step = 20000;
    bigtime_t waited = 0;
    for (;;) {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (!w || w->IsHidden()) continue;
            if (std::strcmp(WindowTitle(w), title) != 0) continue;
            BAlert* a = dynamic_cast<BAlert*>(w);
            if (!a) continue;
            a->Lock();
            BButton* b = a->ButtonAt(which);
            if (b) b->Invoke();
            a->Unlock();
            return b != nullptr;
        }
        if (waited >= timeoutUs) return false;
        snooze(step);
        waited += step;
    }
}

// Put a window that is not the one under test away -- the panel, as its own
// Cancel button does: HIDE it, never Quit it.
static void HideOtherWindows(BWindow* keep) {
    for (int32 i = be_app->CountWindows() - 1; i >= 0; i--) {
        BWindow* w = be_app->WindowAt(i);
        if (w && w != keep && !w->IsHidden()) {
            // LockWithTimeout, never Lock. A window whose looper is quitting
            // can still be listed, and an unbounded Lock() on one blocks
            // forever -- which is a hang in a suite that has a five-minute
            // ctest timeout, not a failure anyone can read. Skipping it is
            // right: it is on its way out.
            if (w->LockWithTimeout(2000000) == B_OK) {
                w->Hide();
                w->Unlock();
            }
        }
    }
    snooze(200000);
}

// Put away whatever a message may have opened, without caring whether it
// opened anything: the point is only to leave a clean screen for the next
// test, and a window that never appeared is not a failure here.
static void MaybeHidePanels(BWindow* keep) {
    snooze(300000);          // let the looper act on the message first
    HideOtherWindows(keep);
}

// --- 1. the harness itself -------------------------------------------------

// A real MainWindow exists and a posted message is handled on its looper: the
// model gains a track. Everything else here rests on this round trip.
static void TestMessageRoundTrip(MainWindow* win, Project& project) {
    std::printf("test_message_round_trip\n");
    const size_t before = project.Tracks().size();
    win->PostMessage(MSG_NEW_MIDI);
    // The FIRST check in the file gets a longer wait than the 5 s default: it
    // is the one that runs against a cold-started window (a fresh build, a
    // cold file cache), and it failed once in a no-LV2 run that passed on a
    // re-run with the same binary. A slow start is not what this asserts.
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const size_t now = project.Tracks().size();
        win->Unlock();
        return now == before + 1;
    }, 20000000));
}

// --- package 32: the CC lane offers ANY controller, and it reaches a voice ---
//
// The lane used to cycle a fixed four-entry list (velocity, vol, expr, pan).
// It now shows any of the 128, picked from the button's menu; the menu is a
// popup a test cannot open, so each item's action (SetLaneCc) is driven
// directly, and then the lane itself is driven through a real click -- a click
// in the strip has to add a controller point and post the region's events, and
// the model's events have to build the expression a voice renders through.
//
// Every touch of the view is under the roll's lock: a BView method belongs to
// its window's thread, and SetLaneCc invalidates, so calling it raw from the
// test thread trips BLooper::check_lock() -- which is a debugger() call, i.e.
// Haiku's crash dialog on the screen and a killed team here (found the hard
// way; the syslog names BView::Invalidate + PianoRollView::SetLaneCc).
static void TestPianoRollCcLane(MainWindow* win, Project& project) {
    std::printf("test_piano_roll_cc_lane\n");
    Track t = MakeMidiTrack(project, { { 60, 100, 0, 8000 } }, "cc-synth");
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
    PianoRollView* rv = nullptr;
    if (roll->LockWithTimeout(1000000) == B_OK) {
        rv = dynamic_cast<PianoRollView*>(roll->FindView("roll"));
        roll->Unlock();
    }
    CHECK(rv != nullptr);
    if (!rv) { roll->Lock(); roll->Quit(); return; }

    // The lane the strip shows (-1 = velocity), through the roll's lock.
    auto setLane = [&](int cc) {
        if (roll->LockWithTimeout(1000000) == B_OK) {
            rv->SetLaneCc(cc);
            roll->Unlock();
        }
    };
    auto laneCc = [&]() -> int {
        int cc = -2;
        if (roll->LockWithTimeout(1000000) == B_OK) {
            cc = rv->LaneCc();
            roll->Unlock();
        }
        return cc;
    };
    auto laneLabel = [&]() -> std::string {
        std::string s;
        if (roll->LockWithTimeout(1000000) == B_OK) {
            s = rv->LaneLabel();
            roll->Unlock();
        }
        return s;
    };

    // It opens on velocity, and every controller number is offered -- including
    // ones no voice acts on yet (74) and the pedal (64). The caption has to
    // agree with the lane, or the strip misnames what it draws.
    CHECK(laneCc() == -1);
    CHECK(laneLabel() == "Vel");
    setLane(64);
    CHECK(laneCc() == 64);
    CHECK(laneLabel() == "Sus");
    setLane(74);
    CHECK(laneCc() == 74);
    CHECK(laneLabel() == "CC74");
    setLane(1);                            // the mod wheel: one a voice DOES act on
    CHECK(laneCc() == 1);
    CHECK(laneLabel() == "Mod");
    Shot("pianoroll-cc-lane");

    // A click in the strip is the whole flow: the view hit-tests the lane
    // (including the snapped frame and the value from the click's height),
    // appends a point, and posts kMsgApplyEvents to the main window. Posted
    // messages are dispatched by the roll's own thread, so the click itself
    // needs no lock -- only converting the point does.
    auto clickLane = [&](float x, float y) {
        BPoint screen(x, y);
        if (roll->LockWithTimeout(1000000) == B_OK) {
            screen = rv->ConvertToScreen(BPoint(x, y));
            roll->Unlock();
        }
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", 1);
            m.AddPoint("where", BPoint(x, y));
            m.AddPoint("screen_where", screen);
            BMessenger(rv).SendMessage(&m);
        }
    };
    // Inside the bottom lane, near its top (a full-height value), at x just
    // right of the keyboard column (frame 0 on the 16th grid).
    BRect b(0, 0, 0, 0);
    if (roll->LockWithTimeout(1000000) == B_OK) {
        b = rv->Bounds();
        roll->Unlock();
    }
    clickLane(60.0f, b.bottom - 60.0f);

    auto laneEvents = [&](std::vector<MidiClipEvent>* out) {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const Track* tr = project.FindTrack(tid);
        const MidiClip* c = tr ? tr->FindMidiClip(cid) : nullptr;
        if (c) *out = c->events;
        win->Unlock();
        return c != nullptr;
    };
    std::vector<MidiClipEvent> events;
    CHECK(WaitFor([&] {
        std::vector<MidiClipEvent> e;
        if (!laneEvents(&e)) return false;
        events = e;
        return !e.empty();
    }));
    bool sawCc1 = false;
    for (const MidiClipEvent& e : events)
        if (e.type == MidiClipEvent::CC && e.data == 1) sawCc1 = true;
    CHECK(sawCc1);                          // the lane's controller, in the model

    // The point the click drew, on screen: the lane's staircase and its handle.
    if (roll->LockWithTimeout(1000000) == B_OK) {
        rv->Invalidate();
        roll->Unlock();
    }
    Shot("pianoroll-cc-point");

    // The lane reaches the VOICES: the model's events are what the engine and
    // the exporter turn into a VoiceExpression (model/MidiExpression.h), so a
    // note rendered through it is not the bare tone any more.
    std::vector<MidiNote> notes;
    if (win->LockWithTimeout(1000000) == B_OK) {
        const Track* tr = project.FindTrack(tid);
        if (tr) notes = tr->CollectPlaybackNotes();
        win->Unlock();
    }
    CHECK(notes.size() == 1);
    Instrument inst;
    inst.waveform = (int)Waveform::Sine;
    inst.attack = 0.005f; inst.decay = 0.0f;
    inst.sustain = 1.0f;  inst.release = 0.0f;
    const size_t n = 4096;
    std::vector<float> plain(n * 2, 0.0f), played(n * 2, 0.0f);
    Synth synth(project.sampleRate);
    const std::vector<BendPoint> tl = BuildBendTimeline(events);
    synth.Render(notes, inst, plain.data(), n, 0, 1.0f);
    synth.Render(notes, inst, played.data(), n, 0,
                 StereoGain{1.0f, 1.0f}, StereoGain{1.0f, 1.0f},
                 ExpressionAt(tl, events, 0, (Frame)n));
    bool differs = false;
    for (size_t i = 0; i < plain.size() && !differs; i++)
        if (std::fabs(played[i] - plain[i]) > 1e-4f) differs = true;
    CHECK(differs);                         // the lane's wheel wobbles the voice

    // Back to velocity, the lane the roll opens on.
    setLane(-1);
    CHECK(laneCc() == -1);
    CHECK(laneLabel() == "Vel");
    roll->Lock();
    roll->Quit();
    snooze(200000);
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
    Shot("pianoroll-window");

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
        if (win->LockWithTimeout(1000000) != B_OK) return false;
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

    std::printf("  export: posting options\n");
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
    std::printf("  export: the panel is up\n");
    Shot("export-panel");
    std::remove(kExportPath);
    CHECK(!FileExists(kExportPath));
    HideOtherWindows(win);
    std::printf("  export: posting the path\n");

    // What that panel posts when a name is chosen.
    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage ref(MSG_EXPORT_REF);
    ref.AddRef("directory", &dir);
    ref.AddString("name", kExportName);
    win->PostMessage(&ref);

    // The window must keep answering while it renders: this track has to land.
    // (Under the lock: the looper is handling the export messages meanwhile.)
    std::printf("  export: waiting for responsiveness\n");
    Shot("export-progress");   // the bar, if the render is still going
    size_t before = 0;
    if (win->LockWithTimeout(1000000) == B_OK) { before = project.Tracks().size(); win->Unlock(); }
    win->PostMessage(MSG_NEW_MIDI);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const size_t now = project.Tracks().size();
        win->Unlock();
        return now == before + 1;
    }));

    CHECK(WaitFor([&] { return FileExists(kExportPath); }, 60000000));
    std::printf("  export: file written\n");
    CHECK(FileExists(kExportPath));
    CHECK(!FileExists(std::string(kExportPath) + ".part"));
    // A finished bounce SAYS so on the status line (the same pass that closes
    // its bar), not only on stderr -- checked before WaitQuiet, because the
    // message is transient on purpose (kStatusHoldUs).
    CHECK(WaitFor([&] {
        const std::string s = TransportStatus(win);
        return s.find("Exported") != std::string::npos
            && s.find("haiku_daw_ui_export.wav") != std::string::npos;
    }, 60000000));
    std::printf("  export: %s\n", TransportStatus(win).c_str());
    CHECK(WaitQuiet());   // its bar closes on a pulse
    std::printf("  export: done\n");
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
    Shot("stems-panel");
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

// --- 4b. the export dialog's format row (M6) -------------------------------

// A bounce that fails raises an alert TITLED "Export" -- the same title as the
// dialog this section looks for. Answer it, or the next title lookup finds the
// alert instead of the window it wants, and the count arithmetic around it
// goes wrong.
static void ClearExportAlert() {
    AnswerAlertWhenUp("Export", 0, 2000000);
    snooze(200000);
}

// The export OPTIONS dialog: a plain BWindow whose title NAMES THE ACTION
// ("Export Mix" for this flow; see TestDialogVocabulary for the stems one), so
// it can no longer be confused with the failure alert, which stays "Export".
//
// The title is shared with the file panel behind it (ShowPanel titles each
// panel for its action), so this lookup is only unambiguous when no panel is
// up -- which is what the WaitQuiet() before it guarantees. Anything that has
// to tell the two apart afterwards compares POINTERS.
static BWindow* FindExportDialog() {
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (!w || w->IsHidden()) continue;
        if (std::strcmp(WindowTitle(w), "Export Mix") != 0) continue;
        if (dynamic_cast<BAlert*>(w) != nullptr) continue;
        return w;
    }
    return nullptr;
}

// Pick a row the way a click in a radio menu does: exactly one marked, so the
// dialog's FindMarked() reads back what was chosen. (BMenuItem::SetMarked does
// not unmark the siblings -- that happens in BMenu's own click handling.)
static void SelectMenuRow(BMenu* menu, int index) {
    if (!menu || index < 0 || index >= menu->CountItems()) return;
    for (int i = 0; i < menu->CountItems(); i++)
        if (BMenuItem* it = menu->ItemAt(i)) it->SetMarked(false);
    if (BMenuItem* it = menu->ItemAt(index)) it->SetMarked(true);
}

// The dialog itself: its Format menu must offer exactly what this build can
// write (AudioFormats' tables), and applying it must reach MainWindow as a
// container. Ends by putting the remembered choice back to plain WAV, so a
// later test that posts only "bits" still bounces a .wav.
static void TestExportFormatDialog(MainWindow* win, Project& project) {
    ClearExportAlert();
    CHECK(WaitQuiet());
    std::printf("test_export_format_dialog\n");
    const int32 windowsBefore = VisibleWindows();
    win->PostMessage(MSG_EXPORT);

    BWindow* dlg = nullptr;
    CHECK(WaitFor([&] { return (dlg = FindExportDialog()) != nullptr; }));
    if (!dlg) return;
    snooze(300000);

    // The dialog the app opened for a MIXDOWN names it (the stems one is
    // checked in TestDialogVocabulary), and its vocabulary is Cancel + Export
    // Mix with Return on the verb.
    CheckVocabulary(dlg, "Export Mix", "ok", "Export Mix");

    // THIS is the window this suite can screenshot. The one MainWindow opens
    // is not mapped by this VM's app_server -- it is in the window list, not
    // hidden, even the ACTIVE window, and it is never drawn (see the record;
    // forcing its workspace mask to the current workspace makes it appear, so
    // the app_server is registering it on a workspace the screen is not
    // showing). The same class built here, the way the piano roll is, draws
    // normally, so what gets reviewed is what the user gets.
    ExportChoices shown;
    ExportWindow* shot = new ExportWindow(BRect(560, 40, 960, 440), shown,
                                          false, BMessenger(win));
    shot->Show();
    snooze(1200000);
    Shot("export-dialog");
    if (shot->LockWithTimeout(2000000) == B_OK) shot->Quit();
    // By identity: the dialog MainWindow opened carries the same title and is
    // still in the list, so a title lookup would never go quiet.
    CHECK(WaitFor([&] { return !WindowListed(shot); }, 5000000));

    // Bounded, like every lock this suite takes outside BAlert: a window whose
    // looper is stuck must fail a check, not hang the run for 15 minutes.
    if (dlg->LockWithTimeout(2000000) != B_OK) return;
    BMenuField* fmt  = dynamic_cast<BMenuField*>(dlg->FindView("fm"));
    BMenuField* qual = dynamic_cast<BMenuField*>(dlg->FindView("vq"));
    CHECK(fmt != nullptr);
    CHECK(qual != nullptr);
    if (fmt && fmt->Menu()) {
        // Rows are the kit-free tables' rows, in order, with no extras: a
        // build without libFLAC must not offer FLAC.
        CHECK(fmt->Menu()->CountItems() == ExportFormatChoiceCount());
        const int n = std::min(fmt->Menu()->CountItems(),
                               ExportFormatChoiceCount());
        for (int i = 0; i < n; i++) {
            BMenuItem* it = fmt->Menu()->ItemAt(i);
            CHECK(it != nullptr);
            if (it) CHECK(std::strcmp(it->Label(),
                                      ExportFormatChoiceAt(i).label) == 0);
        }
        // What a click does in a radio menu: exactly one row marked. (The
        // items carry no message -- applying is the button's job -- so the
        // mark is what the dialog reads back.)
        SelectMenuRow(fmt->Menu(), 1);
    }
    if (qual && qual->Menu()) {
        CHECK(qual->Menu()->CountItems() == VorbisQualityChoiceCount());
        SelectMenuRow(qual->Menu(), VorbisQualityChoiceCount() - 1);
    }
    dlg->Unlock();

    // The dialog applies by posting its own go-ahead to itself; it then quits.
    dlg->PostMessage('exok');
    // Wait for the DIALOG to leave the window list -- BY IDENTITY, not by
    // title: the file panel it opens is titled "Export Mix" too (ShowPanel
    // names each panel for its action, which is where the dialog's own title
    // comes from), so a title lookup can never go quiet once the panel is up.
    // That is also why the shot window above is waited for by pointer.
    CHECK(WaitFor([&] {
        return !WindowListed(dlg) && VisibleWindows() >= windowsBefore + 1;
    }));
    HideOtherWindows(win);
    CHECK(WaitQuiet());

    // Put the remembered choice back to WAV (row 0) for whatever runs next.
    BMessage back(kMsgExportOptions);
    back.AddInt32("container", (int32)AudioFileFormat::Wav);
    back.AddInt32("bits", 16);
    win->PostMessage(&back);
    MaybeHidePanels(win);
}

// One bounce per container this build can write, all the way through the app:
// the dialog's answer, the panel's answer, and the file that lands -- whose
// format is SNIFFED from its bytes, not assumed from the name it was given.
static void TestExportFormats(MainWindow* win, Project& project) {
    ClearExportAlert();
    CHECK(WaitQuiet());
    std::printf("test_export_formats\n");

    // One second of audio, not the eight the suite has accumulated by now.
    // This test is about the CONTAINERS; encoding a long project on the VM's
    // debug build is slow enough (the codecs are unoptimised there) that the
    // first cut of this test blew its own file timeout while the bounce was
    // still running -- and left the export bar up over every test after it.
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.loopEnabled = true;
        project.transport.loopStart   = 0;
        project.transport.loopEnd     = 48000;   // 1 s at the project's rate
        win->Unlock();
    }

    struct Case { AudioFileFormat fmt; const char* name; };
    const Case cases[] = {
        { AudioFileFormat::Wav,  "haiku_daw_ui_fmt.wav" },
        { AudioFileFormat::Flac, "haiku_daw_ui_fmt.flac" },
        { AudioFileFormat::Ogg,  "haiku_daw_ui_fmt.ogg" },
    };
    for (const Case& k : cases) {
        if (!AudioFileFormatCanWrite(k.fmt))
            continue;   // not in this build: the dialog would not offer it
        const std::string path = std::string("/tmp/") + k.name;
        std::remove(path.c_str());
        std::remove((path + ".part").c_str());

        const int32 windowsBefore = VisibleWindows();
        BMessage opts(kMsgExportOptions);
        opts.AddInt32("bits", 16);
        opts.AddInt32("container", (int32)k.fmt);
        opts.AddFloat("vquality", 0.5f);
        opts.AddBool("dither", false);
        opts.AddInt32("rate", 0);
        opts.AddBool("norm", false);
        opts.AddFloat("lufs", -14.0f);
        opts.AddFloat("ceil", -1.0f);
        opts.AddBool("lim", false);
        opts.AddInt32("range", 1);      // the one-second loop window above
        opts.AddInt32("stems", 0);
        win->PostMessage(&opts);
        CHECK(WaitFor([&] { return VisibleWindows() >= windowsBefore + 1; }));
        HideOtherWindows(win);

        entry_ref dir;
        CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
        BMessage ref(MSG_EXPORT_REF);
        ref.AddRef("directory", &dir);
        ref.AddString("name", k.name);
        win->PostMessage(&ref);

        // 60 s per container is ample for one second of audio even with the
        // VM's unoptimised codecs, and still bounds the whole loop so a broken
        // encode fails three checks instead of hanging the suite.
        CHECK(WaitFor([&] { return FileExists(path); }, 60000000));
        CHECK(FileExists(path));
        CHECK(!FileExists(path + ".part"));
        // The bytes decide: a FLAC export is a FLAC whatever it was called.
        CHECK(SniffAudioFileFormat(path) == k.fmt);
        // A failed bounce raises an alert on the pulse. Answer it here rather
        // than letting it sit over the windows every later test counts.
        AnswerAlertWhenUp("Export", 0, 2000000);
        CHECK(WaitQuiet());
        std::remove(path.c_str());
    }

    // Back to WAV for anything that follows (see TestExportFormatDialog).
    BMessage back(kMsgExportOptions);
    back.AddInt32("container", (int32)AudioFileFormat::Wav);
    back.AddInt32("bits", 16);
    win->PostMessage(&back);
    MaybeHidePanels(win);
}

// The loop range bounces what the loop covers, not the whole timeline.
static void TestExportLoopRange(MainWindow* win, Project& project) {
    CHECK(WaitQuiet());
    std::printf("test_export_loop_range\n");
    const char* path = "/tmp/haiku_daw_ui_loop.wav";
    std::remove(path);

    // A loop over the second second of the project (under the lock: the
    // looper reads the transport continuously).
    if (win->LockWithTimeout(1000000) == B_OK) {
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
    if (win->LockWithTimeout(1000000) == B_OK) { project.transport.loopEnabled = false; win->Unlock(); }
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
        if (win->LockWithTimeout(1000000) != B_OK) return false;
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
    //
    // The snapshot is taken BEFORE the op, and the wait comes after it: taking
    // it afterwards races the edit the op posts, which made this check fail
    // once in a handful of runs (observed on the M5 branch's VM pass) -- and
    // humanize draws for each note, so a clamped timing draw plus a zero
    // velocity draw can leave the notes as they were. Retry, so a draw that
    // changes nothing is not a test failure.
    std::vector<MidiNote> before;
    CHECK(notes(&before));
    std::vector<MidiNote> h;
    bool changed = false;
    for (int attempt = 0; attempt < 3 && !changed; attempt++) {
        roll->Lock();
        rv->RunMidiOp(MidiOp::Humanize);
        roll->Unlock();
        changed = WaitFor([&] {
            if (!notes(&h) || h.size() != before.size()) return false;
            return !NotesEqual(h, before);
        });
    }
    CHECK(changed);
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
    Shot("pianoroll-transformed");

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
    Shot("about");
    HideOtherWindows(win);   // the alert, as its OK button does
    std::printf("  version: %s\n", DAW_VERSION_STRING);
    // Non-empty only: the header is generated from CMakeLists, so the drift
    // this guards against (a hand-typed version in the About box) is already
    // impossible -- and a literal here would fail on every version bump for no
    // behavioural reason.
    CHECK(std::strlen(DAW_VERSION_STRING) > 0);
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
    Shot("lv2-native-editor");

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
        if (win->LockWithTimeout(1000000) != B_OK) return false;
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
        if (win->LockWithTimeout(1000000) == B_OK) {
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
        if (win->LockWithTimeout(1000000) == B_OK) {
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
        Shot("lv2-param-panel");
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

// --- 8. M0.2: Save As, silent Save, and New --------------------------------

// The File menu's paths, minus the menu itself (a popup a test cannot open):
// Save As answers the panel and writes the file; Save afterwards writes the
// same path with NO panel and clears the marker; New over a dirty project
// prompts, and Discard leaves an empty, clean, Untitled session.
static void TestFileMenuFlows(MainWindow* win, Project& project,
                              CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_file_menu\n");

    const char* savePath = "/tmp/haiku_daw_ui_save.dawproj";
    std::remove(savePath);

    // Make it dirty so there is something worth saving, and learn how.
    auto dirtyIt = [&] {
        win->PostMessage(MSG_NEW_MIDI);
        return WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool dirty = stack.IsDirty();
            win->Unlock();
            return dirty;
        });
    };

    CHECK(dirtyIt());
    win->PostMessage(MSG_SAVE_AS);
    CHECK(WaitFor([&] { return VisibleWindows() == 2; }));   // the panel
    Shot("save-as-panel");
    HideOtherWindows(win);                    // the panel, as its Cancel does
    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage ref(MSG_SAVE_REF);
    ref.AddRef("directory", &dir);
    ref.AddString("name", "haiku_daw_ui_save.dawproj");
    win->PostMessage(&ref);

    CHECK(WaitFor([&] { return FileExists(savePath); }));
    // ...and it lands clean, with the file's name in the title.
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = !stack.IsDirty()
                     && std::strstr(WindowTitle(win), "haiku_daw_ui_save");
        win->Unlock();
        return ok;
    }));

    // Save is silent now: dirty it again, Save, and NO panel may appear.
    CHECK(dirtyIt());
    win->PostMessage(MSG_SAVE);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool clean = !stack.IsDirty();
        win->Unlock();
        return clean;
    }));
    snooze(300000);                           // a panel would be up by now
    CHECK(VisibleWindows() == 1);
    CHECK(FileExists(savePath));

    // New over the dirty project: the New Project dialog (M3.7) first, then
    // the prompt for the unsaved changes, then Discard.
    CHECK(dirtyIt());
    win->PostMessage(MSG_NEW_PROJECT);
    CHECK(WaitFor([&] { return WindowUp("New Project"); }));
    {
        BWindow* np = FindVisibleWindow("New Project");
        CHECK(np != nullptr);
        if (np != nullptr && np->LockWithTimeout(1000000) == B_OK) {
            NewProjectWindow* chooser = static_cast<NewProjectWindow*>(np);
            chooser->SelectId("empty");
            chooser->PostChosen();     // Create
            np->Unlock();
        }
    }
    CHECK(WaitFor([&] { return AlertUp("Unsaved Changes"); }));
    Shot("unsaved-changes-alert");
    CHECK(AnswerAlertWhenUp("Unsaved Changes", 1));       // Discard
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool empty = project.Tracks().empty() && !stack.IsDirty();
        win->Unlock();
        return empty;
    }));
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const char* title = WindowTitle(win);
        const bool ok = std::strstr(title, "Untitled") && title[0] != '*';
        win->Unlock();
        return ok;
    }));
    std::remove(savePath);
}

// --- 9. M0.1: unsaved changes are tracked and asked about -------------------

// The window half of the dirty tracking (the serial semantics themselves are
// host-tested in commandstack_tests): the title carries the marker, Quit asks
// first, Cancel keeps the session, Save with no path yet opens the panel and
// refuses to quit, and Discard on Open loads the chosen project and lands
// clean with its name in the title. Left for last: it replaces the project.
static void TestUnsavedChanges(MainWindow* win, Project& project,
                               CommandStack& stack) {
    HideOtherWindows(win);   // nothing lingering from earlier tests
    CHECK(WaitQuiet());
    std::printf("test_unsaved_changes\n");

    // Every window lock here is bounded. A prompt holds the window's looper
    // until it is answered; an unbounded Lock() inside a predicate would hang
    // the whole run instead of failing the test (found the hard way), and a
    // prompt wait keyed on window COUNT can be satisfied by an unrelated
    // panel -- so prompts are found by title, never by counting.
    auto lockWin = [&] { return win->LockWithTimeout(1000000) == B_OK; };

    // Dirty the project through the window's own path.
    const size_t before = project.Tracks().size();
    win->PostMessage(MSG_NEW_MIDI);
    CHECK(WaitFor([&] {
        if (!lockWin()) return false;
        const size_t now = project.Tracks().size();
        win->Unlock();
        return now == before + 1;
    }));

    // The title picks the marker up on its own slow poll (not on the 60 Hz
    // pulse, which only runs while the transport does).
    const bool marked = WaitFor([&] {
        if (!lockWin()) return false;
        const bool ok = WindowTitle(win)[0] == '*';
        win->Unlock();
        return ok;
    });
    if (!marked) {   // say what the title actually reads, not just that it failed
        if (lockWin()) {
            std::printf("  title is '%s' (raw '%s', dirty=%d)\n",
                        WindowTitle(win),
                        win->Name() ? win->Name() : "(null)",
                        (int)stack.IsDirty());
            win->Unlock();
        }
    }
    CHECK(marked);

    // Quit -> the prompt. Cancel: the window stays up, still dirty.
    win->PostMessage(B_QUIT_REQUESTED);
    CHECK(WaitFor([&] { return AlertUp("Unsaved Changes"); }));
    CHECK(AnswerAlertWhenUp("Unsaved Changes", 0));       // Cancel
    CHECK(WaitFor([&] { return !AlertUp("Unsaved Changes"); }));
    CHECK(WaitFor([&] { return VisibleWindows() == 1; }));
    bool dirty = false, starred = false;
    CHECK(WaitFor([&] {                          // retried: see the lock note above
        if (!lockWin()) return false;
        dirty   = stack.IsDirty();
        starred = WindowTitle(win)[0] == '*';
        win->Unlock();
        return true;
    }));
    CHECK(dirty);
    CHECK(starred);

    // Quit -> Save, with no path yet: the save panel opens and the window
    // does NOT quit (the action is refused until a path exists).
    win->PostMessage(B_QUIT_REQUESTED);
    CHECK(WaitFor([&] { return AlertUp("Unsaved Changes"); }));
    CHECK(AnswerAlertWhenUp("Unsaved Changes", 2));       // Save
    CHECK(WaitFor([&] { return !AlertUp("Unsaved Changes"); }));
    CHECK(WaitFor([&] { return VisibleWindows() >= 2; }));   // the panel
    Shot("quit-save-panel");
    // Still alive: no quit. Retried, not a single shot -- the window thread
    // may be busy for a moment (an autosave tick), and one slow second is not
    // the failure this is looking for.
    CHECK(WaitFor([&] {
        if (!lockWin()) return false;
        win->Unlock();
        return true;
    }));
    HideOtherWindows(win);                        // the panel, as Cancel does
    CHECK(WaitQuiet());

    // Open, over the dirty project: Discard loads the file, and the loaded
    // file IS the saved state -- clean, with its name in the title.
    const char* openPath = "/tmp/haiku_daw_ui_open.dawproj";
    {
        Project other;
        other.sampleRate = project.sampleRate;
        CHECK(other.AddTrack(MakeMidiTrack(other, { { 60, 100, 0, 4800 } },
                                           "opened-synth")));
        CHECK(ProjectIO::Save(other, openPath));
    }
    entry_ref ref;
    CHECK(BEntry(openPath).GetRef(&ref) == B_OK);
    BMessage open(MSG_OPEN_REF);
    open.AddRef("refs", &ref);
    win->PostMessage(&open);
    CHECK(WaitFor([&] { return AlertUp("Unsaved Changes"); }));
    CHECK(AnswerAlertWhenUp("Unsaved Changes", 1));       // Discard
    CHECK(WaitFor([&] {
        if (!lockWin()) return false;
        const bool loaded = project.Tracks().size() == 1
                         && project.Tracks().front().name == "opened-synth";
        win->Unlock();
        return loaded;
    }));
    CHECK(WaitFor([&] {
        if (!lockWin()) return false;
        const char* title = WindowTitle(win);
        const bool ok = std::strstr(title, "haiku_daw_ui_open") != nullptr
                     && title[0] != '*';
        win->Unlock();
        return ok;
    }));
    bool clean = false;
    CHECK(WaitFor([&] {                          // retried: see the lock note above
        if (!lockWin()) return false;
        clean = !stack.IsDirty();
        win->Unlock();
        return true;
    }));
    CHECK(clean);
    Shot("opened-project");
    std::remove(openPath);
}

// --- 10. M0.4: failures the user can see ------------------------------------

// Two of the new reports, through the handlers that raise them: a save into a
// path that cannot exist, and a load whose media is gone (one dialog, Skip,
// and the clip is left exactly as it was). The recorder's failure report
// cannot be driven here -- the harness has no capture device.
static void TestErrorReports(MainWindow* win, Project& project,
                             CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_error_reports\n");

    // A save that cannot work: the report is the point (the prompt would
    // already have refused to proceed).
    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    BMessage bad(MSG_SAVE_REF);
    bad.AddRef("directory", &dir);
    bad.AddString("name", "no_such_dir_haiku_daw/x.dawproj");
    win->PostMessage(&bad);
    CHECK(WaitFor([&] { return AlertUp("Save Project"); }));
    Shot("save-error-alert");
    HideOtherWindows(win);                    // the alert, as its OK does
    CHECK(WaitQuiet());

    // Clean first: the fixture load below must not have to answer the
    // unsaved-changes prompt (M0.1's own flow covers that).
    const char* cleanPath = "/tmp/haiku_daw_ui_errors.dawproj";
    std::remove(cleanPath);
    BMessage save(MSG_SAVE_REF);
    save.AddRef("directory", &dir);
    save.AddString("name", "haiku_daw_ui_errors.dawproj");
    win->PostMessage(&save);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool clean = !stack.IsDirty();
        win->Unlock();
        return clean;
    }));

    // A load whose clip file is gone: the load succeeds, and ONE dialog
    // offers Locate… or Skip.
    const char* projPath = "/tmp/haiku_daw_ui_missing.dawproj";
    const char* gonePath = "/tmp/haiku_daw_ui_gone.wav";   // never created
    std::remove(projPath);
    std::remove(gonePath);
    {
        Project other;
        other.sampleRate = project.sampleRate;
        Track t;
        t.id = other.NextTrackId();
        t.type = TrackType::Audio;
        t.name = "gone-track";
        Clip c;
        c.id = other.NextClipId();
        c.startFrame = 0;
        c.lengthFrames = 48000;
        c.sourcePath = gonePath;
        t.clips.push_back(c);
        CHECK(other.AddTrack(t));
        CHECK(ProjectIO::Save(other, projPath));
    }
    entry_ref ref;
    CHECK(BEntry(projPath).GetRef(&ref) == B_OK);
    BMessage open(MSG_OPEN_REF);
    open.AddRef("refs", &ref);
    win->PostMessage(&open);
    CHECK(WaitFor([&] { return AlertUp("Missing Media"); }));
    Shot("missing-media-alert");
    const bool skipAnswered = AnswerAlertWhenUp("Missing Media", 0);  // Skip
    std::printf("  skip answered=%d, alert still up=%d, windows=%d\n",
                (int)skipAnswered, (int)AlertUp("Missing Media"),
                (int)VisibleWindows());
    CHECK(skipAnswered);
    const bool skipped = WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        bool ok = false;
        for (const Track& t : project.Tracks())
            if (t.name == "gone-track" && t.clips.size() == 1
                && PathBaseName(t.clips[0].sourcePath) == "haiku_daw_ui_gone.wav")
                ok = true;
        win->Unlock();
        return ok;
    }, 30000000);
    if (!skipped) {
        const status_t locked = win->LockWithTimeout(1000000);
        std::printf("  after Skip: lock=%d, windows=%d, alert up=%d\n",
                    (int)locked, (int)VisibleWindows(),
                    (int)AlertUp("Missing Media"));
        if (locked == B_OK) {
            std::printf("  tracks: %zu\n", project.Tracks().size());
            for (const Track& t : project.Tracks()) {
                std::printf("    '%s' (%zu clips)\n", t.name.c_str(),
                            t.clips.size());
                for (const Clip& c : t.clips)
                    std::printf("      clip path '%s'\n",
                                c.sourcePath.c_str());
            }
            win->Unlock();
        }
    }
    CHECK(skipped);
    // ...and Locate… walks the same fixture: the panel appears, its answer is
    // posted here, and the repair lands as one named undo step.
    const char* foundPath = "/tmp/haiku_daw_ui_found.wav";
    std::remove(foundPath);
    {
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 1000, -1000 };   // 2 stereo frames
        CHECK(w.Open(foundPath, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
    }
    win->PostMessage(&open);                       // the fixture again
    CHECK(WaitFor([&] { return AlertUp("Missing Media"); }));
    CHECK(AnswerAlertWhenUp("Missing Media", 1));  // Locate…
    CHECK(WaitFor([&] { return VisibleWindows() == 2; }));   // the panel
    Shot("relink-panel");
    entry_ref found;
    CHECK(BEntry(foundPath).GetRef(&found) == B_OK);
    BMessage picked(MSG_RELINK_REF);
    picked.AddRef("refs", &found);
    win->PostMessage(&picked);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        bool ok = false;
        for (const Track& t : project.Tracks())
            if (t.name == "gone-track" && t.clips.size() == 1
                && PathBaseName(t.clips[0].sourcePath)
                       == "haiku_daw_ui_found.wav")
                ok = true;
        win->Unlock();
        return ok;
    }, 30000000));
    CHECK(stack.UndoName() == "Locate Missing Media");
    HideOtherWindows(win);
    CHECK(WaitQuiet());

    std::remove(projPath);
    std::remove(foundPath);
    std::remove(cleanPath);
}

// --- 12. M0.7: the transport keys and the timeline's focus ------------------

// Space reaches the timeline without clicking it first; a text field keeps its
// own keys (a space is a space there); a click on the timeline takes the focus.
// Needs a project with content: an empty one has nothing to play (StartPlayback
// stops again), which is a different behaviour entirely.
static void TestKeyboardFocus(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_keyboard_focus\n");

    // Something the engine can actually PLAY: the previous test's fixture
    // points at a file it removes on the way out, and a project whose only clip
    // is gone makes StartPlayback fail (correctly) -- which the first version
    // of this test read as "the key did nothing".
    {
        const char* wav = "/tmp/haiku_daw_ui_key.wav";
        std::remove(wav);
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 4000, -4000 };
        CHECK(w.Open(wav, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
        Track t;
        t.id = project.NextTrackId();
        t.type = TrackType::Audio;
        t.name = "keys-track";
        Clip c;
        c.id = project.NextClipId();
        c.startFrame = 0;
        c.lengthFrames = 48000;
        c.sourcePath = wav;
        t.clips.push_back(c);
        CHECK(LockedAddTrack(win, project, t));
    }

    auto playing = [&] {
        bool p = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            p = win->IsPlaying();
            win->Unlock();
        }
        return p;
    };
    auto sendKey = [&](const char* bytes) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    };

    // A click on the timeline takes the focus (and with it the keys). The
    // message goes to the view itself -- the way this harness posts everything
    // -- because a window-level posted mouse message is not hit-tested onto
    // the view under its "where" (the focus stayed on the tempo field's
    // _input_ text view when it was posted to the window).
    BView* timeline = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        timeline = win->FindView("timeline");
        win->Unlock();
    }
    CHECK(timeline != nullptr);
    BMessage down(B_MOUSE_DOWN);
    down.AddInt32("buttons", 1);
    down.AddPoint("where", BPoint(220, 14));   // the ruler, in view coordinates
    if (timeline) BMessenger(timeline).SendMessage(&down);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = dynamic_cast<TimelineView*>(win->CurrentFocus())
                            != nullptr;
        win->Unlock();
        return ok;
    }));

    // Space toggles the transport...
    CHECK(!playing());
    sendKey(" ");
    CHECK(WaitFor([&] { return playing(); }));
    Shot("playing");
    sendKey(" ");
    CHECK(WaitFor([&] { return !playing(); }));

    // ...but with the tempo field focused it is text, not transport.
    BView* tempo = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tempo = win->FindView("tempo");
        if (tempo) tempo->MakeFocus(true);
        win->Unlock();
    }
    CHECK(tempo != nullptr);
    sendKey(" ");
    snooze(400000);
    CHECK(!playing());

    // Clicking the timeline again takes the focus back from the field.
    if (timeline) BMessenger(timeline).SendMessage(&down);
    bool focused = WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = dynamic_cast<TimelineView*>(win->CurrentFocus())
                            != nullptr;
        win->Unlock();
        return ok;
    });
    if (!focused && win->LockWithTimeout(1000000) == B_OK) {
        BView* f = win->CurrentFocus();
        BView* tl = win->FindView("timeline");
        std::printf("  focus after click: '%s' (timeline '%s' at %p)\n",
                    f && f->Name() ? f->Name() : "(null)",
                    tl && tl->Name() ? tl->Name() : "(null)", (void*)tl);
        win->Unlock();
    }
    CHECK(focused);
    sendKey(" ");
    CHECK(WaitFor([&] { return playing(); }));
    sendKey(" ");                              // leave the transport stopped
    CHECK(WaitFor([&] { return !playing(); }));

    // --- the UX-shell rule: only a TEXT FIELD holds the map -----------------
    // Clicking a slider (the master volume) or a list used to take the whole
    // keyboard map with it: R/S/M/Home/,/. and the arrows did nothing until
    // the user clicked the timeline back. The map owns its keys whatever
    // control has the focus; only a key the map does NOT bind stays with the
    // focus view.
    BView* master = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        master = win->FindView("master");
        win->Unlock();
    }
    CHECK(master != nullptr);
    if (master != nullptr) {
        if (win->LockWithTimeout(1000000) == B_OK) {
            master->MakeFocus(true);
            win->Unlock();
        }
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->CurrentFocus() == master;
            win->Unlock();
            return ok;
        }));

        auto playhead = [&] {
            daw::Frame ph = -1;
            if (win->LockWithTimeout(1000000) == B_OK) {
                ph = project.transport.playhead;
                win->Unlock();
            }
            return ph;
        };
        // Home reaches the transport: the playhead goes to the start.
        if (win->LockWithTimeout(1000000) == B_OK) {
            project.transport.playhead = 96000;
            win->Unlock();
        }
        sendKey("\x01");   // B_HOME
        CHECK(WaitFor([&] { return playhead() == 0; }));

        // M drops a marker (the map's Editing action, not the slider's).
        auto markerCount = [&] {
            size_t n = 0;
            if (win->LockWithTimeout(1000000) == B_OK) {
                n = project.markers.size();
                win->Unlock();
            }
            return n;
        };
        const size_t before = markerCount();
        sendKey("m");
        CHECK(WaitFor([&] { return markerCount() == before + 1; }));

        // The slider itself still has its value (the map does not touch it),
        // and an UNBOUND key is still the focused control's: 'z' does nothing
        // to the transport.
        const bool stillStopped = !playing();
        sendKey("z");
        snooze(200000);
        CHECK(stillStopped && !playing());
    }

    // Put the focus back on the timeline for whatever runs next.
    if (timeline) BMessenger(timeline).SendMessage(&down);
    snooze(150000);
}

// M1.2 slice B: every shared layout metric is a function of the theme scale
// (which follows the user's font size), and the view's hit-testing follows the
// geometry it draws. The boundary check drives that through a real window: a
// click just below the 150% ruler bottom must seek, and the SAME y at 100% is
// lane content and must not -- one of the two answers flips if any part of the
// chain keeps a fixed 28 instead of asking RulerHeight().
static void TestThemeScale(MainWindow* win, Project& project) {
    std::printf("theme scale ...\n");

    // Two empty audio lanes at the end, so selection can be read back.
    Track a, b;
    a.id = project.NextTrackId(); a.type = TrackType::Audio;
    a.name = "scale-a"; a.gain = 1.0f;
    b.id = project.NextTrackId(); b.type = TrackType::Audio;
    b.name = "scale-b"; b.gain = 1.0f;
    CHECK(LockedAddTrack(win, project, a));
    CHECK(LockedAddTrack(win, project, b));

    BView* tlView = nullptr;
    size_t iA = 0;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tlView = win->FindView("timeline");
        iA = project.Tracks().size() - 2;
        win->Unlock();
    }
    CHECK(tlView != nullptr);
    if (!tlView) return;

    // Top of the lane stack, whatever the earlier flows scrolled to (the
    // scroll clamps at 0).
    BMessage wheel(B_MOUSE_WHEEL_CHANGED);
    wheel.AddFloat("be:wheel_delta_y", -100000.0f);
    BMessenger(tlView).SendMessage(&wheel);
    snooze(120000);

    // A synthetic mouse message needs BOTH points: the window derives the
    // view-relative point it hands to MouseDown from "screen_where" (a message
    // posted to the view with only "where" arrives as (0,0)).
    auto click = [&](float x, float y) {
        BPoint screen(x, y);
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tlView->ConvertToScreen(BPoint(x, y));
            win->Unlock();
        }
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddPoint("where", BPoint(x, y));
            m.AddPoint("screen_where", screen);
            BMessenger(tlView).SendMessage(&m);
        }
    };
    auto selected = [&] {
        TrackId id = kInvalidTrackId;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* tv =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                id = tv->SelectedTrack();
            win->Unlock();
        }
        return id;
    };
    auto playhead = [&] {
        Frame f = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            f = project.transport.playhead;
            win->Unlock();
        }
        return f;
    };

    // --- 150%: the metrics scale ...
    SetThemeScaleOverride(1.5f);
    CHECK(std::fabs(TrackHeight()    - 111.0f) < 0.01f);
    CHECK(std::fabs(RulerHeight()    -  42.0f) < 0.01f);
    CHECK(std::fabs(ToolbarHeight()  -  42.0f) < 0.01f);
    CHECK(std::fabs(TrackGap()       -   1.5f) < 0.01f);
    CHECK(std::fabs(HeaderWidth()    - 225.0f) < 0.01f);
    CHECK(std::fabs(InspectorWidth() - 285.0f) < 0.01f);

    // ... and the view draws and hit-tests with them: a click in the middle of
    // the ruler seeks. The y is the ruler's own middle through Themed(), so it
    // lands in the ruler at any scale -- and outside it (in a lane) at 100%,
    // which the second half of this test uses.
    const float x = HeaderWidth() + Themed(50.0f);
    const float yRuler = ToolbarHeight() + RulerHeight() * 0.5f;
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 0;
        win->Unlock();
    }
    click(x, yRuler);
    CHECK(WaitFor([&] { return playhead() > 0; }));

    // A lane click at the scaled geometry selects that lane (the click target
    // and the drawn lane are one computation).
    const float yLaneA = TimelineContentTop()
                       + (float)iA * (TrackHeight() + TrackGap())
                       + TrackHeight() * 0.5f;
    click(Themed(2.0f), yLaneA);   // header gutter, left of the M/S/R buttons
    CHECK(WaitFor([&] { return selected() == a.id; }));
    const float yLaneB = yLaneA + TrackHeight() + TrackGap();
    click(Themed(2.0f), yLaneB);
    CHECK(WaitFor([&] { return selected() == b.id; }));
    Shot("theme-150");

    // --- back at 100% the same y is lane content, not ruler: no seek.
    SetThemeScaleOverride(1.0f);
    CHECK(std::fabs(TrackHeight() - 74.0f) < 0.01f);
    CHECK(std::fabs(RulerHeight() - 28.0f) < 0.01f);
    CHECK(std::fabs(ToolbarHeight() - 28.0f) < 0.01f);
    CHECK(std::fabs(TimelineContentTop() - 56.0f) < 0.01f);
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 0;
        win->Unlock();
    }
    click(x, yRuler);
    snooze(200000);
    CHECK(playhead() == 0);

    SetThemeScaleOverride(0.0f);   // back to the real font
}

// M2.1-2.3: the arrange window's feel. The tool palette and its keys, the snap
// grid and its indicator, split-at-playhead, each tool's gesture, the pointer
// feedback (hit zones and the cursor each one wants), the mouse-anchored zoom,
// scrolling past the end, the real scrollbars and the piano roll following the
// playhead.
static void TestArrangeFeel(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_arrange_feel\n");

    // A MIDI track of its own at the end, with one 2-bar region far enough in
    // that there is empty lane on both sides of it. 120 BPM @ 48k: a beat is
    // 24000 frames, so the region is beats 2..4.
    Track t;
    t.id = project.NextTrackId();
    t.type = TrackType::Midi;
    t.name = "arrange";
    MidiClip mc;
    mc.id = project.NextClipId();
    mc.startFrame   = 48000;
    mc.lengthFrames = 48000;
    mc.notes.push_back({ 60, 100, 2000, 1000 });
    t.midiClips.push_back(mc);
    CHECK(LockedAddTrack(win, project, t));

    // ...and an AUDIO track under it, because two of the pointer feed-backs
    // (gain, slip) are audio-only by design: asking for them over a MIDI region
    // asks for a gesture that does not exist there. A real (tiny) wav, so the
    // engine can load it if a later test plays the project.
    const char* kArrangeWav = "/tmp/haiku_daw_ui_arrange.wav";
    {
        std::remove(kArrangeWav);
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 3000, -3000 };
        CHECK(w.Open(kArrangeWav, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
    }
    Track ta;
    ta.id = project.NextTrackId();
    ta.type = TrackType::Audio;
    ta.name = "arrange-audio";
    Clip ac;
    ac.id = project.NextClipId();
    ac.startFrame   = 48000;
    ac.lengthFrames = 48000;
    ac.sourcePath   = kArrangeWav;
    ta.clips.push_back(ac);
    CHECK(LockedAddTrack(win, project, ta));

    TimelineView* tv = nullptr;
    int laneIdx = -1;        // the MIDI track
    int laneIdxAudio = -1;   // the audio track under it
    bool dockWasOpen = false;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv = dynamic_cast<TimelineView*>(win->FindView("timeline"));
        laneIdx = project.IndexOfTrack(t.id);
        laneIdxAudio = project.IndexOfTrack(ta.id);
        dockWasOpen = win->FindView("dock") != nullptr;
        win->Unlock();
    }
    CHECK(tv != nullptr);
    if (!tv) return;
    CHECK(laneIdx >= 0);

    // Focus the timeline the way a user would; every bare key below arrives
    // through MainWindow's router only because of this.
    auto click = [&](float x, float y) {
        BPoint screen(x, y);
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(BPoint(x, y));
            win->Unlock();
        }
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", 1);
            m.AddPoint("where", BPoint(x, y));
            m.AddPoint("screen_where", screen);
            BMessenger(tv).SendMessage(&m);
        }
    };
    auto postMouse = [&](uint32 what, BPoint at, uint32 mods = 0) {
        BPoint screen = at;
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(at);
            win->Unlock();
        }
        BMessage m(what);
        m.AddInt32("buttons", what == B_MOUSE_UP ? 0 : 1);
        m.AddInt32("clicks", 1);
        m.AddInt32("modifiers", (int32)mods);
        m.AddPoint("where", at);
        m.AddPoint("screen_where", screen);
        BMessenger(tv).SendMessage(&m);
    };
    auto sendKey = [&](const char* bytes) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    };
    // Read something out of the view under the window lock.
    auto readTool = [&] {
        int idx = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                idx = (int)v->ActiveTool();
            win->Unlock();
        }
        return idx;
    };
    auto regions = [&] {
        int n = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            n = tr ? (int)tr->midiClips.size() : -1;
            win->Unlock();
        }
        return n;
    };
    // EVERY view call runs under the window lock. BView's accessors --
    // Bounds(), and anything that invalidates or sets a value -- call
    // check_lock(), which DEBUGGERS on an unlocked looper: on the target that
    // is a crash dialog, not a failed check (this test's first version faulted
    // exactly here, calling HitTest without the lock).
    auto hitAt = [&](const BPoint& p) {
        TimelineView::Hit h;
        if (win->LockWithTimeout(1000000) == B_OK) {
            h = tv->HitTest(p);
            win->Unlock();
        }
        return h;
    };
    auto frameX = [&](Frame f) {
        float x = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            x = tv->FrameToX(f);
            win->Unlock();
        }
        return x;
    };

    // Top of the lane stack, whatever earlier flows scrolled to.
    BMessage topWheel(B_MOUSE_WHEEL_CHANGED);
    topWheel.AddFloat("be:wheel_delta_y", -100000.0f);
    BMessenger(tv).SendMessage(&topWheel);
    snooze(120000);

    // The lane's geometry, as the view computes it. The track is the last one
    // added and earlier tests have filled the stack, so scroll to the BOTTOM of
    // it first: at 0 the lane would be below the pane and a synthetic click's
    // "screen_where" would not land on the view at all.
    BMessage bottomWheel(B_MOUSE_WHEEL_CHANGED);
    bottomWheel.AddFloat("be:wheel_delta_y", 100000.0f);
    BMessenger(tv).SendMessage(&bottomWheel);
    snooze(120000);
    float scrollY = 0.0f;
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
            scrollY = v->ScrollY();
        win->Unlock();
    }
    const float laneTop = TimelineContentTop() - scrollY
                        + (float)laneIdx * (TrackHeight() + TrackGap());
    const float laneMid = laneTop + TrackHeight() * 0.7f;   // below the top band
    const float laneTopAudio = TimelineContentTop() - scrollY
                             + (float)laneIdxAudio * (TrackHeight() + TrackGap());
    const float laneMidAudio = laneTopAudio + TrackHeight() * 0.7f;
    const float audioLeft  = frameX(ac.startFrame);
    const float audioMid   = frameX(ac.startFrame + ac.lengthFrames / 2);
    const float clipLeft  = frameX(mc.startFrame);
    const float clipRight = frameX(mc.startFrame + mc.lengthFrames);
    // The whole lane must be on screen, or the clicks below are not delivered.
    float viewH = 0.0f;
    if (win->LockWithTimeout(1000000) == B_OK) {
        viewH = tv->Bounds().Height();
        win->Unlock();
    }
    CHECK(laneTop >= TimelineContentTop());
    CHECK(laneTop + TrackHeight() <= viewH + 1.0f);

    // Focus the timeline the way a user would -- a FULL click on this track's
    // empty lane. A lone mouse-down (the first version) left a rubber-band
    // gesture or a clip drag hanging; the next click's mouse-up then committed a
    // band-select over a zero-height rect, which SELECTED a clip nobody meant
    // and silently restricted split-at-playhead to it. The click must also leave
    // an empty selection, which every section below assumes.
    click(frameX(mc.startFrame + 60000), laneMid);   // empty lane, in view
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        TimelineView* v = dynamic_cast<TimelineView*>(win->CurrentFocus());
        const bool ok = v != nullptr && v->SelectionCount() == 0;
        win->Unlock();
        return ok;
    }));

    // --- 1. The tool palette: the keys select, and the palette button does too.
    std::printf("  arrange: tools\n");
    sendKey("3");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Scissors; }));
    sendKey("5");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Mute; }));
    sendKey("1");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Pointer; }));
    // The Glue button (4th of six, at the roll's button pitch).
    click(Themed(4.0f) + 3 * Themed(24.0f) + Themed(11.0f), Themed(14.0f));
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Glue; }));
    Shot("arrange-palette");
    sendKey("1");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Pointer; }));

    // --- 2. Pointer feedback: the hit zones and the cursor each one wants.
    std::printf("  arrange: hit zones and cursors\n");
    {
        struct Expect { float x, y; TimelineView::Zone zone; const char* what; };
        const Expect zones[] = {
            { clipLeft + Themed(2.0f), laneTop + Themed(5.0f),
              TimelineView::Zone::FadeIn,  "top-left corner fades" },
            { clipLeft + Themed(2.0f), laneMid,
              TimelineView::Zone::TrimLeft, "left edge trims" },
            { clipRight - Themed(2.0f), laneMid,
              TimelineView::Zone::TrimRight, "right edge trims" },
            { (clipLeft + clipRight) * 0.5f, laneMid,
              TimelineView::Zone::Body, "the middle moves" },
        };
        for (const Expect& e : zones) {
            const TimelineView::Hit h = hitAt(BPoint(e.x, e.y));
            std::printf("  hit[%s] lane=%d zone=%d\n", e.what, h.lane,
                        (int)h.zone);
            CHECK(h.track == t.id);
            CHECK(h.clip == mc.id);
            CHECK(h.zone == e.zone);
        }
        // The cursor follows the tool and the modifiers (M2.1's set: trim,
        // fade, gain, move, split, slip).
        const BPoint mid((clipLeft + clipRight) * 0.5f, laneMid);
        const BPoint left(clipLeft + Themed(2.0f), laneMid);
        // The audio clip's body and edge: the gain and slip zones live there.
        const BPoint audioBody(audioMid, laneMidAudio);
        const BPoint audioLeft2(audioLeft + Themed(2.0f), laneMidAudio);
        auto cursorAt = [&](const BPoint& p, uint32 mods, int tool) {
            TimelineView::Pointer c = TimelineView::Pointer::Default;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (TimelineView* v =
                        dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                    v->SetTool((TimelineView::Tool)tool);
                    c = v->CursorFor(p, mods);
                }
                win->Unlock();
            }
            return c;
        };
        CHECK(cursorAt(mid, 0, 0) == TimelineView::Pointer::Move);
        CHECK(cursorAt(left, 0, 0) == TimelineView::Pointer::Trim);
        CHECK(cursorAt(BPoint(left.x, laneTop + Themed(5.0f)), 0, 0)
              == TimelineView::Pointer::Fade);
        CHECK(cursorAt(audioBody, B_CONTROL_KEY, 0) == TimelineView::Pointer::Gain);
        CHECK(cursorAt(audioBody, B_OPTION_KEY, 0) == TimelineView::Pointer::Slip);
        CHECK(cursorAt(audioLeft2, 0, 0) == TimelineView::Pointer::Trim);
        CHECK(cursorAt(audioBody, 0, 0) == TimelineView::Pointer::Move);
        CHECK(cursorAt(mid, 0, 2) == TimelineView::Pointer::Split);   // scissors
        CHECK(cursorAt(mid, 0, 5) == TimelineView::Pointer::Fade);    // fade tool
        // Restore the pointer tool through the same path.
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                v->SetTool(TimelineView::Tool::Pointer);
            win->Unlock();
        }
        // APPLYING a cursor is a different path from mapping one, and it is
        // where this feature first crashed: the tool glyphs are drawn into a
        // bitmap, and a bitmap made without B_BITMAP_ACCEPTS_VIEWS has no
        // off-screen window, so its drawing view has no owner and every BView
        // call debuggers ("View method requires owner and doesn't have one") --
        // a crash dialog on the target, not a failed check. Hover, then switch
        // to each tool, so every glyph cursor is really applied.
        for (int tool = 1; tool <= 5; tool++) {      // pencil..fade
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (TimelineView* v =
                        dynamic_cast<TimelineView*>(win->FindView("timeline")))
                    v->SetTool((TimelineView::Tool)tool);
                win->Unlock();
            }
            postMouse(B_MOUSE_MOVED, BPoint(mid.x, mid.y));
            snooze(100000);
        }
        // ...and the slip cursor, which is the pointer tool with Alt held.
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                v->SetTool(TimelineView::Tool::Pointer);
            win->Unlock();
        }
        postMouse(B_MOUSE_MOVED, BPoint(mid.x, mid.y), B_OPTION_KEY);
        snooze(120000);
        {
            int tool = -1;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (TimelineView* v =
                        dynamic_cast<TimelineView*>(win->FindView("timeline")))
                    tool = (int)v->ActiveTool();
                win->Unlock();
            }
            CHECK(tool == (int)TimelineView::Tool::Pointer);   // still alive
        }
        // The hover highlight is drawn for this state: park the pointer on the
        // clip's left edge and let the view repaint.
        postMouse(B_MOUSE_MOVED, left);
        snooze(150000);
        Shot("arrange-hover");
    }

    // --- 3. Split at the playhead (S), one undo step.
    std::printf("  arrange: split at playhead\n");
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 72000;   // beat 3, inside the region
        win->Unlock();
    }
    sendKey("s");
    CHECK(WaitFor([&] { return regions() == 2; }));
    {
        Frame a = -1, b = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            if (tr && tr->midiClips.size() == 2) {
                a = tr->midiClips[0].startFrame + tr->midiClips[0].lengthFrames;
                b = tr->midiClips[1].startFrame;
            }
            win->Unlock();
        }
        CHECK(a == 72000);            // the cut landed on the playhead
        CHECK(b == 72000);            // and the halves meet exactly
    }
    // One gesture, one undo entry: undo puts the single region back.
    win->PostMessage(MSG_UNDO_TEST);  // Edit > Undo
    CHECK(WaitFor([&] { return regions() == 1; }));
    sendKey("s");                     // ...and it can be done again
    CHECK(WaitFor([&] { return regions() == 2; }));

    // --- 4. Scissors cuts where it is clicked; Glue puts it back.
    std::printf("  arrange: scissors and glue\n");
    sendKey("3");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Scissors; }));
    click(frameX(54000), laneMid);          // inside the first half
    CHECK(WaitFor([&] { return regions() == 3; }));
    {
        // The cut is where the scissors were, not at an edge or the playhead.
        Frame a = -1, b = -1, len = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            if (tr && tr->midiClips.size() == 3) {
                a = tr->midiClips[0].startFrame + tr->midiClips[0].lengthFrames;
                b = tr->midiClips[1].startFrame;
                len = tr->midiClips[0].lengthFrames;
            }
            win->Unlock();
        }
        CHECK(a == 54000);
        CHECK(b == 54000);
        CHECK(len == 6000);
    }
    sendKey("4");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Glue; }));
    click(frameX(50000), laneMid);          // glue the halves back
    CHECK(WaitFor([&] { return regions() == 2; }));
    {
        // ...and the glued region is the one they came from, note and all.
        Frame start = -1, len = 0; std::size_t notes = 0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            if (tr && !tr->midiClips.empty()) {
                start = tr->midiClips[0].startFrame;
                len   = tr->midiClips[0].lengthFrames;
                notes = tr->midiClips[0].notes.size();
            }
            win->Unlock();
        }
        CHECK(start == 48000);
        CHECK(len == 24000);
        CHECK(notes == 1);
    }
    Shot("arrange-clips");

    // --- 5. Pencil draws a region on empty lane, snapped to the grid.
    std::printf("  arrange: pencil\n");
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (TimelineView* v =
                dynamic_cast<TimelineView*>(win->FindView("timeline")))
            v->SetSnapGrid({ SnapKind::Quarter, false });
        win->Unlock();
    }
    sendKey("2");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Pencil; }));
    const float penX0 = frameX(110000);     // past the region's end
    const float penX1 = frameX(170000);
    postMouse(B_MOUSE_DOWN, BPoint(penX0, laneMid));
    postMouse(B_MOUSE_MOVED, BPoint(penX1, laneMid));
    postMouse(B_MOUSE_UP, BPoint(penX1, laneMid));
    CHECK(WaitFor([&] { return regions() == 3; }));
    {
        Frame start = -1, len = 0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            if (tr)
                for (const MidiClip& c : tr->midiClips)
                    if (c.startFrame > 100000) { start = c.startFrame;
                                                 len = c.lengthFrames; }
            win->Unlock();
        }
        CHECK(start == 120000);     // 110000 snapped to the quarter-note grid
        CHECK(len == 48000);        // 170000 snapped to 168000, minus 120000
    }

    // --- 6. Mute mutes the BLOCK under the pointer (M2.5). The previous
    // package's boundary note said the tool then muted the TRACK because that
    // was the only mute the model had; clip depth gave clips and regions their
    // own, and this is the tool's resolved meaning. The track's own M button
    // stays the track mute (checked in the header section of the suite).
    std::printf("  arrange: mute\n");
    sendKey("5");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Mute; }));
    auto regionMuted = [&] {
        bool muted = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            if (tr && !tr->midiClips.empty())
                for (const MidiClip& c : tr->midiClips)
                    if (c.startFrame == 48000) muted = c.muted;
            win->Unlock();
        }
        return muted;
    };
    auto trackMuted = [&] {
        bool muted = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Track* tr = project.FindTrack(t.id);
            muted = tr && tr->muted;
            win->Unlock();
        }
        return muted;
    };
    click(frameX(60000), laneMid);
    CHECK(WaitFor([&] { return regionMuted(); }));
    CHECK(!trackMuted());          // the TRACK's mute is untouched
    click(frameX(60000), laneMid);
    CHECK(WaitFor([&] { return !regionMuted(); }));

    // --- 7. Fade sets the fade of the nearer edge, without the corner grip.
    std::printf("  arrange: fade\n");
    sendKey("6");
    CHECK(WaitFor([&] { return readTool() == (int)TimelineView::Tool::Fade; }));
    const float midX = frameX(55000);
    postMouse(B_MOUSE_DOWN, BPoint(midX, laneMid));
    postMouse(B_MOUSE_MOVED, BPoint(midX + Themed(40.0f), laneMid));
    postMouse(B_MOUSE_UP, BPoint(midX + Themed(40.0f), laneMid));
    CHECK(WaitFor([&] {
        Frame fin = 0, len = 0, start = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* tr = project.FindTrack(t.id))
                for (const MidiClip& c : tr->midiClips)
                    if (c.startFrame == 48000) {
                        fin = c.fadeInFrames; len = c.lengthFrames;
                        start = c.startFrame;
                    }
            win->Unlock();
        }
        return fin > 0 && len == 24000 && start == 48000;
    }));
    Shot("arrange-fade");

    // --- 8. The snap field says what is selected, and Off means free.
    std::printf("  arrange: snap indicator\n");
    {
        const char* label = nullptr;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                label = v->SnapLabel();
            win->Unlock();
        }
        CHECK(label != nullptr && std::strcmp(label, "1/4") == 0);
    }
    // A ruler click seeks to the grid (a quarter note), then freely with Off.
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 0;
        win->Unlock();
    }
    click(frameX(20000), ToolbarHeight() + RulerHeight() * 0.5f);
    CHECK(WaitFor([&] {
        bool ok = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            ok = project.transport.playhead == 24000;   // snapped to beat 1
            win->Unlock();
        }
        return ok;
    }));
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 0;
        if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
            v->SetSnapGrid({ SnapKind::Off, false });
        win->Unlock();
    }
    {
        const char* label = nullptr;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v =
                    dynamic_cast<TimelineView*>(win->FindView("timeline")))
                label = v->SnapLabel();
            win->Unlock();
        }
        CHECK(label != nullptr && std::strcmp(label, "Off") == 0);
    }
    Shot("arrange-snap");
    click(frameX(20000), ToolbarHeight() + RulerHeight() * 0.5f);
    CHECK(WaitFor([&] {
        bool ok = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            // Within a few frames of the click, and nowhere near a beat line.
            const Frame ph = project.transport.playhead;
            ok = std::llabs(ph - 20000) <= 4;
            win->Unlock();
        }
        return ok;
    }));
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
            v->SetSnapGrid({ SnapKind::Sixteenth, false });
        win->Unlock();
    }

    // --- 9. The zoom anchors on the pointer (Ctrl+wheel) ...
    std::printf("  arrange: zoom anchor (pointer)\n");
    {
        // Scroll away from the content's start first: at scroll 0 a zoom-in
        // cannot keep an anchor put (the scroll has nowhere to go but 0). Not
        // too far, either: the scroll ceiling is the content end (the
        // scroll-past-the-end rule), and a zoom that would cross it is clamped,
        // which moves the anchor -- correct behaviour, but not what this checks.
        const float anchorX = HeaderWidth() + Themed(300.0f);
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
                v->ScrollToFrame(50000);
            win->Unlock();
        }
        postMouse(B_MOUSE_MOVED, BPoint(anchorX, laneMid));
        snooze(80000);
        Frame anchor = 0;
        double fppBefore = 0.0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                anchor = v->XToFrame(anchorX);
                fppBefore = v->FramesPerPixel();
            }
            win->Unlock();
        }
        // Haiku's wheel delta is positive rolling DOWN (toward the user), so
        // up (negative) zooms in -- fewer frames per pixel -- and down zooms
        // out. Both directions are driven, and the anchor must survive both.
        auto ctrlWheel = [&](float delta) {
            BMessage zoom(B_MOUSE_WHEEL_CHANGED);
            zoom.AddFloat("be:wheel_delta_y", delta);
            zoom.AddInt32("modifiers", B_CONTROL_KEY);
            BMessenger(tv).SendMessage(&zoom);
        };
        auto fpp = [&] {
            double f = 0.0;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
                    f = v->FramesPerPixel();
                win->Unlock();
            }
            return f;
        };
        auto anchorError = [&] {
            float x = 0.0f;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
                    x = v->FrameToX(anchor);
                win->Unlock();
            }
            return std::fabs(x - anchorX);
        };

        ctrlWheel(-1.0f);                       // wheel up: zoom in
        CHECK(WaitFor([&] { return fpp() < fppBefore; }));
        std::printf("  zoom in: fpp %.0f -> %.0f, anchor off by %.1f\n",
                    fppBefore, fpp(), anchorError());
        CHECK(anchorError() < 2.0f);
        const double zoomedIn = fpp();
        ctrlWheel(1.0f);                        // wheel down: back out
        CHECK(WaitFor([&] { return fpp() > zoomedIn; }));
        std::printf("  zoom out: fpp %.0f -> %.0f, anchor off by %.1f\n",
                    zoomedIn, fpp(), anchorError());
        CHECK(anchorError() < 2.0f);
    }

    // --- 10. ... and on the playhead for the keyboard's +/-.
    std::printf("  arrange: zoom anchor (playhead)\n");
    {
        Frame ph = 0;
        float xBefore = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            ph = project.transport.playhead;
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline")))
                xBefore = v->FrameToX(ph);
            win->Unlock();
        }
        // Park the playhead where the view can see it.
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                v->ScrollToFrame(50000);         // the playhead in view, and
                project.transport.playhead = 100000;   // the scroll off 0
                v->SetPlayhead(100000);
                ph = 100000;
                xBefore = v->FrameToX(ph);
            }
            win->Unlock();
        }
        sendKey("-");   // zoom out
        snooze(150000);
        float xAfter = 0.0f;
        double fpp = 0.0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                xAfter = v->FrameToX(ph);
                fpp = v->FramesPerPixel();
            }
            win->Unlock();
        }
        CHECK(fpp > 0.0);
        CHECK(std::fabs(xAfter - xBefore) < 3.0f);   // the playhead stayed put
    }

    // --- 11. Scrolling past the end of the last clip, with real scrollbars.
    std::printf("  arrange: scrollbars and past-the-end\n");
    {
        Frame lastEnd = 0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            for (const Track& tr : project.Tracks()) {
                for (const Clip& c : tr.clips)
                    if (c.startFrame + c.lengthFrames > lastEnd)
                        lastEnd = c.startFrame + c.lengthFrames;
                for (const MidiClip& c : tr.midiClips)
                    if (c.startFrame + c.lengthFrames > lastEnd)
                        lastEnd = c.startFrame + c.lengthFrames;
            }
            win->Unlock();
        }
        CHECK(lastEnd > 0);
        Frame scrolled = -1, end = 0, clipEndX = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                v->ScrollToFrame(lastEnd * 4);       // far past the content
                scrolled = v->ScrollFrame();
                end = v->ContentEndFrame();
                clipEndX = v->FrameToX(lastEnd);
            }
            win->Unlock();
        }
        CHECK(scrolled == end);          // the clamp is the content end...
        CHECK(scrolled >= lastEnd);      // ...which is past the last clip
        CHECK(clipEndX <= HeaderWidth());   // it really scrolled off the left
        Shot("arrange-scrollbars");

        // The scrollbars are real controls with the content's range.
        BScrollBar* hbar = nullptr;
        BScrollBar* vbar = nullptr;
        float rangeMax = -1.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            hbar = dynamic_cast<BScrollBar*>(win->FindView("tlhscroll"));
            vbar = dynamic_cast<BScrollBar*>(win->FindView("tlvscroll"));
            if (hbar) {
                float mn = 0.0f;
                hbar->GetRange(&mn, &rangeMax);
            }
            win->Unlock();
        }
        CHECK(hbar != nullptr);
        CHECK(vbar != nullptr);
        CHECK(std::fabs(rangeMax - (float)end) < 2.0f);
        if (hbar) {
            // Send the thumb to the far end first, so the drag below is a real
            // change: BScrollBar::SetValue ignores a value it is already at.
            float barBefore = -1.0f, barAfter = -1.0f, scrollAfter = -1.0f;
            if (win->LockWithTimeout(1000000) == B_OK) {
                hbar->SetValue(rangeMax);
                barBefore = hbar->Value();
                hbar->SetValue(0.0f);
                barAfter = hbar->Value();
                if (TimelineView* v =
                        dynamic_cast<TimelineView*>(win->FindView("timeline")))
                    scrollAfter = (float)v->ScrollFrame();
                win->Unlock();
            }
            std::printf("  hbar: %.0f -> %.0f, scroll now %.0f\n",
                        barBefore, barAfter, scrollAfter);
            CHECK(barAfter == 0.0f);
            CHECK(WaitFor([&] {
                bool ok = false;
                if (win->LockWithTimeout(1000000) == B_OK) {
                    if (TimelineView* v =
                            dynamic_cast<TimelineView*>(win->FindView("timeline")))
                        ok = v->ScrollFrame() == 0;
                    win->Unlock();
                }
                return ok;
            }));
        }
    }

    // --- 12. The docked piano roll follows the playhead into view.
    std::printf("  arrange: roll follow\n");
    {
        BMessage open(kMsgOpenEditor);
        open.AddInt64("track", (int64)t.id);
        open.AddInt64("clip", (int64)t.midiClips.empty()
                                  ? kInvalidClipId : t.midiClips[0].id);
        win->PostMessage(&open);
        PianoRollView* roll = nullptr;
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            roll = dynamic_cast<PianoRollView*>(win->FindView("roll"));
            win->Unlock();
            return roll != nullptr;
        }));
        if (roll) {
            Frame scroll0 = 0, span = 0;
            if (win->LockWithTimeout(1000000) == B_OK) {
                scroll0 = roll->ScrollFrame();
                span    = roll->VisibleSpan();
                win->Unlock();
            }
            CHECK(span > 0);
            const Frame far = mc.startFrame + span * 4;   // well past the view
            if (win->LockWithTimeout(1000000) == B_OK) {
                roll->SetPlayhead(far);
                win->Unlock();
            }
            Frame scroll1 = 0;
            if (win->LockWithTimeout(1000000) == B_OK) {
                scroll1 = roll->ScrollFrame();
                win->Unlock();
            }
            std::printf("  roll follow: scroll %lld -> %lld (span %lld)\n",
                        (long long)scroll0, (long long)scroll1, (long long)span);
            CHECK(scroll1 != scroll0);                       // it followed
            const Frame rel = far - mc.startFrame;
            CHECK(scroll1 <= rel && rel <= scroll1 + span);  // it is in view
            Shot("arrange-roll-follow");
        }
    }

    // Leave the window roughly as it was found. The test AFTER this one measures
    // this window (the big-project play-start budget), and an open dock pane, a
    // non-pointer tool and a far-out horizontal scroll are not part of that
    // baseline.
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (TimelineView* v = dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
            v->SetTool(TimelineView::Tool::Pointer);
            v->ScrollToFrame(0);
        }
        win->Unlock();
    }
    bool dockOpen = false;
    if (win->LockWithTimeout(1000000) == B_OK) {
        dockOpen = win->FindView("dock") != nullptr;
        win->Unlock();
    }
    if (dockOpen != dockWasOpen) win->PostMessage(MSG_TOGGLE_DOCK);
    snooze(200000);
    if (win->LockWithTimeout(1000000) == B_OK) {
        const bool nowOpen = win->FindView("dock") != nullptr;
        win->Unlock();
        CHECK(nowOpen == dockWasOpen);   // left as it was found
    }
}

// ---------------------------------------------------------------------------
// M7.2 — time-stretch. The Cmd/Alt right-edge drag stretches a clip to fit
// instead of trimming it: the ratio is derived from the span the clip already
// references (never from the file's length), the whole gesture is one undo
// step, and the clip draws the ratio it plays at.
static void TestClipStretch(MainWindow* win, Project& project,
                            CommandStack* stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_clip_stretch\n");

    // A 2 s source and a clip cut out of its MIDDLE: 0.5 s of timeline
    // referencing the 0.5 s of material that starts 1 s in. Dragged out to 1 s
    // of timeline the ratio must be 2.0 — the clipped span stretched — and not
    // 1.0, which is what dividing by the file's own length would give.
    const char* kPath = "/tmp/haiku_daw_ui_stretch.wav";
    const Frame kFile = 96000, kOffset = 48000, kLen = 24000;
    {
        std::remove(kPath);
        WavWriter w;
        CHECK(w.OpenFormat(kPath, 48000, 2, 16, false));
        std::vector<int16_t> d((size_t)kFile * 2, 0);
        for (size_t i = 0; i < (size_t)kFile; i++) {
            const int16_t v = (int16_t)(6000.0
                * std::sin(2.0 * M_PI * 220.0 * (double)i / 48000.0));
            d[i * 2 + 0] = v;
            d[i * 2 + 1] = v;
        }
        CHECK(w.WriteInt16(d.data(), (ssize_t)d.size()));
        CHECK(w.Close());
    }

    Track ta;
    ta.id   = project.NextTrackId();
    ta.type = TrackType::Audio;
    ta.name = "stretch";
    Clip ac;
    ac.id           = project.NextClipId();
    ac.startFrame   = 48000;      // 1.0 s in (a 16th grid line)
    ac.lengthFrames = kLen;
    ac.sourceOffset = kOffset;
    ac.sourcePath   = kPath;
    ta.clips.push_back(ac);
    CHECK(LockedAddTrack(win, project, ta));
    snooze(120000);

    TimelineView* tv = nullptr;
    int laneIdx = -1;
    float scrollY = 0.0f;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv = dynamic_cast<TimelineView*>(win->FindView("timeline"));
        laneIdx = project.IndexOfTrack(ta.id);
        if (tv) scrollY = tv->ScrollY();
        win->Unlock();
    }
    CHECK(tv != nullptr);
    CHECK(laneIdx >= 0);
    if (!tv || laneIdx < 0) return;

    auto frameX = [&](Frame f) { return tv->FrameToX(f); };
    const float laneTop = TimelineContentTop() - scrollY
                        + (float)laneIdx * (TrackHeight() + TrackGap());
    const float laneMid = laneTop + TrackHeight() * 0.7f;   // below the top band

    auto postMouse = [&](uint32 what, BPoint at, uint32 mods) {
        BPoint screen = at;
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(at);
            win->Unlock();
        }
        BMessage m(what);
        m.AddInt32("buttons", what == B_MOUSE_UP ? 0 : 1);
        m.AddInt32("clicks", 1);
        m.AddInt32("modifiers", (int32)mods);
        m.AddPoint("where", at);
        m.AddPoint("screen_where", screen);
        BMessenger(tv).SendMessage(&m);
        snooze(30000);
    };
    auto clipState = [&](Frame* len, double* ratio, Frame* off) {
        *len = -1; *ratio = -1.0; *off = -1;
        if (win->LockWithTimeout(1000000) != B_OK) return;
        if (const Track* t = project.FindTrack(ta.id))
            if (const Clip* c = t->FindClip(ac.id)) {
                *len = c->lengthFrames; *ratio = c->stretchRatio;
                *off = c->sourceOffset;
            }
        win->Unlock();
    };

    // Cmd-drag the right edge from the clip's end (72000) out to 96000 — one
    // bar further at 120 BPM — which asks for a 48000-frame clip over the same
    // material.
    const BPoint edge(frameX(ac.startFrame + kLen) - Themed(3.0f), laneMid);
    const BPoint out(frameX(ac.startFrame + 48000), laneMid);
    postMouse(B_MOUSE_DOWN, edge, B_COMMAND_KEY);
    postMouse(B_MOUSE_MOVED, out, B_COMMAND_KEY);
    postMouse(B_MOUSE_UP, out, B_COMMAND_KEY);

    Frame len = 0, off = 0; double ratio = 0.0;
    clipState(&len, &ratio, &off);
    std::printf("    dragged: len %lld ratio %.4f offset %lld\n",
                (long long)len, ratio, (long long)off);
    CHECK(len == 48000);
    CHECK(std::fabs(ratio - 2.0) < 1e-6);
    CHECK(off == kOffset);                      // the span's start is untouched
    if (win->LockWithTimeout(1000000) == B_OK) {
        CHECK(stack->UndoName() == "Stretch Clip");   // one step for the drag
        win->Unlock();
    }

    // Undo puts the geometry AND the ratio back together; redo re-applies both.
    if (win->LockWithTimeout(1000000) == B_OK) {
        stack->Undo(project);
        win->Unlock();
    }
    snooze(60000);
    clipState(&len, &ratio, &off);
    CHECK(len == kLen);
    CHECK(ratio == 1.0);
    if (win->LockWithTimeout(1000000) == B_OK) {
        stack->Redo(project);
        win->Unlock();
    }
    snooze(60000);
    clipState(&len, &ratio, &off);
    CHECK(len == 48000);
    CHECK(std::fabs(ratio - 2.0) < 1e-6);

    // Shot for the screenshot pass: the clip is stretched, so its name strip
    // carries the ratio badge.
    win->PostMessage(kMsgUiRefresh);
    snooze(120000);
    Shot("clip-stretch");

    // The plain right-edge drag still TRIMS: same length change, no ratio.
    const BPoint edge2(frameX(ac.startFrame + 48000) - Themed(3.0f), laneMid);
    const BPoint in(frameX(ac.startFrame + 36000), laneMid);
    postMouse(B_MOUSE_DOWN, edge2, 0);
    postMouse(B_MOUSE_MOVED, in, 0);
    postMouse(B_MOUSE_UP, in, 0);
    clipState(&len, &ratio, &off);
    std::printf("    trimmed: len %lld ratio %.4f\n", (long long)len, ratio);
    CHECK(len == 36000);
    CHECK(std::fabs(ratio - 2.0) < 1e-6);       // trimming does not re-derive it

    // Leave the project as it was found: two undos (the trim, then the
    // stretch) put the clip back to its authored length and ratio.
    if (win->LockWithTimeout(1000000) == B_OK) {
        stack->Undo(project);
        stack->Undo(project);
        win->Unlock();
    }
    snooze(60000);
    clipState(&len, &ratio, &off);
    CHECK(len == kLen);
    CHECK(ratio == 1.0);
}

// ---------------------------------------------------------------------------
// M2.4 (time-range selection and its edit operations) + M2.5 (clip depth).
//
// Everything here is driven through the real paths: the range by the same
// mouse messages a pointer sends, the Edit menu by the messages its items
// post, the clip-depth actions by the methods the context menu runs, and the
// child-window Cmd-Z by a key message to that window's looper.
static void TestArrangeDepth(MainWindow* win, Project& project,
                             CommandStack* stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_arrange_depth\n");

    // Fixture: one audio track with a 1-second clip and one MIDI track with a
    // region, both starting at 48000 (beat 2 at 120 BPM / 48k).
    const char* kWav = "/tmp/haiku_daw_ui_depth.wav";
    {
        std::remove(kWav);
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 2500, -2500 };
        CHECK(w.Open(kWav, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
    }
    Track ta;
    ta.id = project.NextTrackId();
    ta.type = TrackType::Audio;
    ta.name = "depth";
    Clip ac;
    ac.id = project.NextClipId();
    ac.startFrame = 48000;
    ac.lengthFrames = 48000;
    ac.sourcePath = kWav;
    ta.clips.push_back(ac);
    CHECK(LockedAddTrack(win, project, ta));

    Track tm;
    tm.id = project.NextTrackId();
    tm.type = TrackType::Midi;
    tm.name = "depth-midi";
    MidiClip mc;
    mc.id = project.NextClipId();
    mc.startFrame = 48000;
    mc.lengthFrames = 48000;
    mc.notes.push_back({ 60, 100, 0, 1000 });
    tm.midiClips.push_back(mc);
    CHECK(LockedAddTrack(win, project, tm));

    TimelineView* tv = nullptr;
    int laneIdx = -1, laneIdxMidi = -1;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv = dynamic_cast<TimelineView*>(win->FindView("timeline"));
        laneIdx = project.IndexOfTrack(ta.id);
        laneIdxMidi = project.IndexOfTrack(tm.id);
        win->Unlock();
    }
    CHECK(tv != nullptr);
    if (!tv) return;

    auto click = [&](float x, float y) {
        BPoint screen(x, y);
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(BPoint(x, y));
            win->Unlock();
        }
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", 1);
            m.AddPoint("where", BPoint(x, y));
            m.AddPoint("screen_where", screen);
            BMessenger(tv).SendMessage(&m);
        }
    };
    auto postMouse = [&](uint32 what, BPoint at, uint32 mods = 0) {
        BPoint screen = at;
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(at);
            win->Unlock();
        }
        BMessage m(what);
        m.AddInt32("buttons", what == B_MOUSE_UP ? 0 : 1);
        m.AddInt32("clicks", 1);
        m.AddInt32("modifiers", (int32)mods);
        m.AddPoint("where", at);
        m.AddPoint("screen_where", screen);
        BMessenger(tv).SendMessage(&m);
    };
    auto sendKey = [&](const char* bytes) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    };
    auto frameX = [&](Frame f) {
        float x = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            x = tv->FrameToX(f);
            win->Unlock();
        }
        return x;
    };
    auto range = [&](Frame* a, Frame* b) {
        bool has = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            has = tv->HasTimeRange();
            *a = tv->RangeStart();
            *b = tv->RangeEnd();
            win->Unlock();
        }
        return has;
    };
    auto clipOf = [&](TrackId tid, ClipId cid) {
        Clip c;
        bool found = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* t = project.FindTrack(tid))
                if (const Clip* p = t->FindClip(cid)) { c = *p; found = true; }
            win->Unlock();
        }
        (void)found;
        return c;
    };
    auto clips = [&](TrackId tid) {
        std::size_t n = 0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* t = project.FindTrack(tid)) n = t->clips.size();
            win->Unlock();
        }
        return n;
    };
    auto undoName = [&] {
        std::string s;
        if (win->LockWithTimeout(1000000) == B_OK) {
            s = stack->UndoName();
            win->Unlock();
        }
        return s;
    };
    // The sections above end at whatever zoom their last wheel or zoom key
    // left (TestArrangeFeel finishes with a zoom-out), and a click on the
    // fixture clip has to land on its BODY — 9 px from an edge is a trim grab,
    // and the in-place rename needs a strip wide enough to hold a field. Zoom
    // in until the clip is comfortably wide and put the top of the timeline
    // back in view, so a click's x is where the clip is drawn. Returns the
    // zoom it found, so the caller can put it back (the sections after this
    // one were written at the old scale).
    auto zoomForBodyClick = [&]() -> double {
        double fpp = 0.0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            fpp = tv->FramesPerPixel();
            win->Unlock();
        }
        const double was = fpp;
        for (int i = 0; i < 10 && fpp > 128.0; i++) {
            if (win->LockWithTimeout(1000000) == B_OK) {
                tv->ZoomBy(0.5);
                fpp = tv->FramesPerPixel();
                win->Unlock();
            }
        }
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->ScrollToFrame(0);
            win->Unlock();
        }
        snooze(100000);
        return was;
    };
    auto restoreZoom = [&](double was) {
        double now = 0.0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            now = tv->FramesPerPixel();
            if (was > 0.0 && now > 0.0 && was != now) tv->ZoomBy(was / now);
            win->Unlock();
        }
    };
    // The Edit menu, by walking the bar (FindMenu exists only on BMenu).
    auto editMenu = [&]() -> BMenu* {
        BMenu* out = nullptr;
        if (BMenuBar* mb = win->KeyMenuBar())
            for (int32 i = 0; i < mb->CountItems(); i++)
                if (BMenuItem* it = mb->ItemAt(i)) {
                    const char* l = it->Label();
                    if (l && std::strcmp(l, "Edit") == 0) out = it->Submenu();
                }
        return out;
    };
    auto editMenuLabel = [&](int index) {
        std::string label;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (BMenu* m = editMenu())
                if (BMenuItem* it = m->ItemAt(index)) {
                    const char* l = it->Label();
                    label = l ? l : "";
                }
            win->Unlock();
        }
        return label;
    };
    auto editMenuEnabled = [&](int index) {
        bool on = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (BMenu* m = editMenu())
                if (BMenuItem* it = m->ItemAt(index)) on = it->IsEnabled();
            win->Unlock();
        }
        return on;
    };

    // Scroll the new tracks into view (they are the last two).
    BMessage bottomWheel(B_MOUSE_WHEEL_CHANGED);
    bottomWheel.AddFloat("be:wheel_delta_y", 100000.0f);
    BMessenger(tv).SendMessage(&bottomWheel);
    snooze(120000);
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv->ScrollToFrame(0);
        win->Unlock();
    }
    snooze(120000);
    float scrollY = 0.0f;
    if (win->LockWithTimeout(1000000) == B_OK) {
        scrollY = tv->ScrollY();
        win->Unlock();
    }
    const float laneTopA = TimelineContentTop() - scrollY
                         + (float)laneIdx * (TrackHeight() + TrackGap());
    const float laneMidA = laneTopA + TrackHeight() * 0.7f;
    const float laneTopM = TimelineContentTop() - scrollY
                         + (float)laneIdxMidi * (TrackHeight() + TrackGap());
    const float laneMidM = laneTopM + TrackHeight() * 0.7f;
    CHECK(laneTopA >= TimelineContentTop());

    // Focus the timeline with a full click on empty lane content, which also
    // leaves the range and the selection empty.
    click(frameX(200000), laneMidA);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = dynamic_cast<TimelineView*>(win->CurrentFocus()) != nullptr;
        win->Unlock();
        return ok;
    }));

    // --- 1. A drag on empty lane content makes a time range ----------------
    std::printf("  depth: range drag\n");
    {
        // The state this section STARTS from, stated rather than asserted: the
        // bare click above clears the range (that gesture is checked on purpose
        // in section 2, where a range is set first), but a synthetic click can
        // be joined by the app_server's own report of the real pointer — the
        // press sets SetMouseEventMask(B_POINTER_EVENTS), so a pointer report
        // that lands between the press and the release is, to the view, an
        // ordinary range drag. That race would set a range HERE and fail a line
        // whose subject is the drag below, not the click. (Seen once on the VM:
        // the merged branch's run failed this check while section 2's identical
        // click passed in the same run.)
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->ClearTimeRange();
            win->Unlock();
        }
        Frame a = -1, b = -1;
        CHECK(!range(&a, &b));          // ...and it really is clear now
        // The drag itself: press, move, release -- the pointer events a mouse
        // sends, with the release far enough for the 4 px threshold. It must
        // start on EMPTY content: a press inside a clip is a clip drag (which
        // is what the first cut of this test did, and it quietly moved the
        // fixture 30000 frames right for every section after it).
        const float x0 = frameX(200000), x1 = frameX(212000);
        postMouse(B_MOUSE_DOWN, BPoint(x0, laneMidA));
        postMouse(B_MOUSE_MOVED, BPoint(x1, laneMidA));
        postMouse(B_MOUSE_UP, BPoint(x1, laneMidA));
        CHECK(WaitFor([&] { return range(&a, &b); }));
        // Snapped to the 1/16 grid (6000 frames at this tempo).
        CHECK(a == 198000);
        CHECK(b == 210000);
        // ... and the clip under the drag is untouched: this was a range, not
        // a clip move.
        CHECK(clipOf(ta.id, ac.id).startFrame == ac.startFrame);
        Shot("depth-range");   // the band over the lanes + the ruler marks
    }

    // --- 2. A bare click on empty content clears it again ------------------
    {
        click(frameX(200000), laneMidA);
        CHECK(WaitFor([&] {
            Frame a = -1, b = -1;
            return !range(&a, &b);
        }));
    }

    // --- 3. Alt-drag on the ruler sets the range from above ---------------
    std::printf("  depth: range from the ruler\n");
    {
        const float yRuler = ToolbarHeight() + RulerHeight() * 0.5f;
        postMouse(B_MOUSE_DOWN, BPoint(frameX(100000), yRuler), B_OPTION_KEY);
        postMouse(B_MOUSE_MOVED, BPoint(frameX(130000), yRuler), B_OPTION_KEY);
        postMouse(B_MOUSE_UP, BPoint(frameX(130000), yRuler), B_OPTION_KEY);
        Frame a = -1, b = -1;
        CHECK(WaitFor([&] { return range(&a, &b); }));
        CHECK(a == 102000 && b == 132000);   // snapped to the 1/16 grid
        Shot("depth-range-ruler");
        click(frameX(200000), laneMidA);     // clear again for the next section
        CHECK(WaitFor([&] { Frame a = -1, b = -1; return !range(&a, &b); }));
    }

    // --- 4. The Edit menu says what Undo will undo -------------------------
    std::printf("  depth: edit menu labels\n");
    {
        // Index 0 = Undo, 1 = Redo in the Edit menu.
        CHECK(editMenuLabel(0).rfind("Undo", 0) == 0);
        // A model edit through the stack renames it, and the 2 Hz title poll
        // is what picks it up (the timeline's own edits never message the
        // window). Nudge is the cheapest edit to trigger here.
        {
            if (win->LockWithTimeout(1000000) == B_OK) {
                tv->SelectAllClips();     // selects both fixture blocks
                win->Unlock();
            }
        }
        sendKey(".");
        CHECK(WaitFor([&] { return undoName() == "Nudge Right"; }, 5000000));
        CHECK(WaitFor([&] { return editMenuLabel(0) == "Undo Nudge Right"; },
                      5000000));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return editMenuLabel(0) != "Undo Nudge Right"; }));
        Shot("depth-edit-menu");
    }

    // --- 5. Range copy -> paste at the playhead ---------------------------
    std::printf("  depth: copy and paste\n");
    {
        // Range over the first half of the audio clip.
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTimeRange(48000, 72000);
            win->Unlock();
        }
        win->PostMessage(MSG_EDIT_COPY);
        CHECK(WaitFor([&] {
            bool ok = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                ok = tv->ClipboardCount() >= 2;   // this clip + the region
                win->Unlock();
            }
            return ok;
        }));
        const std::size_t before = clips(ta.id);
        {
            if (win->LockWithTimeout(1000000) == B_OK) {
                project.transport.playhead = 300000;
                win->Unlock();
            }
        }
        win->PostMessage(MSG_EDIT_PASTE);
        // Earlier fixtures' clips overlap this range too (they share the frame
        // range), so the count grows by more than one; what matters is that
        // THIS clip's piece landed at the playhead, as a fresh clip, 24000 long
        // (the range's length) and read from the clip's own first sample.
        CHECK(WaitFor([&] { return clips(ta.id) > before; }));
        {
            const Clip* pasted = nullptr;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(ta.id))
                    for (const Clip& c : t->clips)
                        if (c.startFrame == 300000
                            && c.lengthFrames == 24000) pasted = &c;
                win->Unlock();
            }
            CHECK(pasted != nullptr);
            if (pasted) {
                CHECK(pasted->sourceOffset == 0);   // from the clip's start
                CHECK(pasted->id != ac.id);         // a fresh id
            }
        }
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clips(ta.id) == before; }));
    }

    // --- 6. Range delete splits/trims what it covers, and undoes ----------
    std::printf("  depth: range delete\n");
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTimeRange(60000, 72000);   // strictly inside the clip
            win->Unlock();
        }
        win->PostMessage(MSG_EDIT_DELETE);
        // The clip covers the range: it becomes a left half that keeps its id
        // and a fresh right half, and nothing sounds between them.
        CHECK(WaitFor([&] { return clips(ta.id) == 2; }));
        {
            const Clip left = clipOf(ta.id, ac.id);
            CHECK(left.startFrame == 48000 && left.lengthFrames == 12000);
            bool rightOk = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(ta.id))
                    for (const Clip& c : t->clips)
                        if (c.id != ac.id)
                            rightOk = (c.startFrame == 72000
                                       && c.lengthFrames == 24000
                                       && c.sourceOffset == 24000);
                win->Unlock();
            }
            CHECK(rightOk);
        }
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] {
            const Clip c = clipOf(ta.id, ac.id);
            return c.startFrame == 48000 && c.lengthFrames == 48000
                && c.sourceOffset == 0 && clips(ta.id) == 1;
        }));
    }

    // --- 6b. Delete acts on the thing the user touched LAST (item C) -------
    // The range used to win whenever one existed, even after a click on a
    // clip: the Delete key then removed a time range the user had stopped
    // looking at. The two selections are mutually exclusive now — the clip
    // click drops the range — so Delete takes the clip.
    std::printf("  depth: delete acts on the newest selection\n");
    double zoomWas = 0.0;
    {
        zoomWas = zoomForBodyClick();
        const float xMid = (frameX(48000) + frameX(96000)) * 0.5f;  // clip's middle
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTimeRange(60000, 72000);
            win->Unlock();
        }
        Frame ra = -1, rb = -1;
        CHECK(WaitFor([&] { return range(&ra, &rb); }));

        click(xMid, laneMidA);               // on the clip: selects it...
        CHECK(WaitFor([&] {                  // ...and drops the range
            Frame a = -1, b = -1;
            return !range(&a, &b);
        }));
        bool sel = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            sel = tv->SelectionCount() > 0;
            win->Unlock();
        }
        CHECK(sel);
        CHECK(clips(ta.id) == 1);            // the click changed nothing else

        win->PostMessage(MSG_EDIT_DELETE);   // Edit > Delete / the Delete key
        CHECK(WaitFor([&] { return clips(ta.id) == 0; }));
        CHECK(undoName() == "Delete Clips");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clips(ta.id) == 1; }));
        CHECK(clipOf(ta.id, ac.id).lengthFrames == 48000);   // whole clip back
        // The range is gone, not merely hidden: none of the clip was cut away.
        CHECK(clipOf(ta.id, ac.id).startFrame == 48000);
    }

    // --- 7. Insert Silence ripples everything from the range start --------
    std::printf("  depth: insert silence\n");
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTimeRange(60000, 72000);   // range length 12000
            win->Unlock();
        }
        win->PostMessage(MSG_EDIT_INSERT_SIL);
        // The clip started before the point: it splits, and the right half
        // (60000..96000, 36000 long) lands 12000 further right.
        CHECK(WaitFor([&] { return clips(ta.id) == 2; }));
        {
            const Clip left = clipOf(ta.id, ac.id);
            CHECK(left.startFrame == 48000 && left.lengthFrames == 12000);
            bool moved = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(ta.id))
                    for (const Clip& c : t->clips)
                        if (c.id != ac.id)
                            moved = (c.startFrame == 72000
                                     && c.lengthFrames == 36000
                                     && c.sourceOffset == 12000);
                win->Unlock();
            }
            CHECK(moved);
        }
        // The region straddles the point too: it splits the same way, and the
        // right half lands one range length later.
        {
            bool regionOk = false;
            int regions = 0;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(tm.id)) {
                    regions = (int)t->midiClips.size();
                    for (const MidiClip& c : t->midiClips)
                        if (c.id != mc.id && c.startFrame == 72000
                            && c.lengthFrames == 36000)
                            regionOk = true;
                }
                win->Unlock();
            }
            CHECK(regions == 2);
            CHECK(regionOk);
        }
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clips(ta.id) == 1; }));
        CHECK(clipOf(ta.id, ac.id).lengthFrames == 48000);
    }

    // --- 8. Select All, Duplicate, Esc ------------------------------------
    std::printf("  depth: select all and duplicate\n");
    {
        win->PostMessage(MSG_EDIT_SELECT_ALL);
        CHECK(WaitFor([&] {
            bool ok = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                ok = tv->SelectionCount() >= 2;
                win->Unlock();
            }
            return ok;
        }));
        {
            // Duplicate (by label: the separators shift the indices) lights up
            // when there is something to duplicate.
            bool dupOn = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (BMenu* m = editMenu())
                    for (int32 i = 0; i < m->CountItems(); i++)
                        if (BMenuItem* it = m->ItemAt(i)) {
                            const char* l = it->Label();
                            if (l && std::strcmp(l, "Duplicate") == 0)
                                dupOn = it->IsEnabled();
                        }
                win->Unlock();
            }
            CHECK(dupOn);
        }
        const std::size_t before = clips(ta.id);
        win->PostMessage(MSG_EDIT_DUPLICATE);
        CHECK(WaitFor([&] { return clips(ta.id) == before + 1; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clips(ta.id) == before; }));
        sendKey("\x1b");   // Esc: drop the selection
        CHECK(WaitFor([&] {
            bool ok = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                ok = tv->SelectionCount() == 0;
                win->Unlock();
            }
            return ok;
        }));
    }

    // --- 9. The Mute tool mutes the block (M2.5) --------------------------
    std::printf("  depth: clip mute\n");
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTool(TimelineView::Tool::Mute);
            win->Unlock();
        }
        const float x = frameX(60000);
        click(x, laneMidA);
        CHECK(WaitFor([&] {
            const Clip c = clipOf(ta.id, ac.id);
            return c.muted;
        }));
        // The track's own mute stays put (it is the header M button's).
        {
            bool trackMuted = true;
            if (win->LockWithTimeout(1000000) == B_OK) {
                const Track* t = project.FindTrack(ta.id);
                trackMuted = t && t->muted;
                win->Unlock();
            }
            CHECK(!trackMuted);
        }
        Shot("depth-clip-muted");
        click(x, laneMidA);
        CHECK(WaitFor([&] { return !clipOf(ta.id, ac.id).muted; }));
        // The same tool mutes a MIDI region.
        click(frameX(60000), laneMidM);
        CHECK(WaitFor([&] {
            bool muted = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(tm.id))
                    if (const MidiClip* c = t->FindMidiClip(mc.id))
                        muted = c->muted;
                win->Unlock();
            }
            return muted;
        }));
        click(frameX(60000), laneMidM);
        CHECK(WaitFor([&] {
            bool muted = true;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (const Track* t = project.FindTrack(tm.id))
                    if (const MidiClip* c = t->FindMidiClip(mc.id))
                        muted = c->muted;
                win->Unlock();
            }
            return !muted;
        }));
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetTool(TimelineView::Tool::Pointer);
            win->Unlock();
        }
    }

    // --- 10. Clip name (RenameWindow's own message) and colour ------------
    std::printf("  depth: clip name and colour\n");
    {
        // RenameWindow's OK posts exactly this to the timeline.
        BMessage rename(kMsgRenameClip);
        rename.AddInt64("track", (int64)ta.id);
        rename.AddInt64("aux", (int64)ac.id);
        rename.AddString("name", "Verse");
        BMessenger(tv).SendMessage(&rename);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).name == "Verse"; }));
        CHECK(undoName() == "Rename Clip");   // and it is one undo step
        Shot("depth-clip-named");

        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->NextClipColour(ta.id, ac.id);   // what the menu entry runs
            win->Unlock();
        }
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).colorIndex == 1; }));
        Shot("depth-clip-colour");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).colorIndex == 0; }));
    }

    // --- 10b. Rename a clip IN PLACE, in its name strip (package 50) -------
    // The double-click used to open RenameWindow at the raw click point, which
    // can hang off the screen; the field lives on the clip now, and the keys
    // (Enter commits, Esc cancels) are the track rename's own.
    std::printf("  depth: clip rename in place\n");
    {
        const float xc = (frameX(48000) + frameX(96000)) * 0.5f;   // clip's middle
        BPoint screen(xc, laneMidA);
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = tv->ConvertToScreen(BPoint(xc, laneMidA));
            win->Unlock();
        }
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", 2);
            m.AddInt32("modifiers", 0);
            m.AddPoint("where", BPoint(xc, laneMidA));
            m.AddPoint("screen_where", screen);
            BMessenger(tv).SendMessage(&m);
        }
        CHECK(WaitFor([&] {
            bool on = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                on = tv->RenamingInPlace() && tv->RenamingClip() == ac.id;
                win->Unlock();
            }
            return on;
        }));
        Shot("depth-clip-rename-field");      // the open field, on the strip
        for (int i = 0; i < 5; i++) sendKey("\x08");   // clear "Verse"
        sendKey("C"); sendKey("h"); sendKey("1");
        sendKey("\n");                        // Enter commits
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).name == "Ch1"; }));
        CHECK(undoName() == "Rename Clip");
        CHECK(WaitFor([&] {
            bool done = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                done = !tv->RenamingInPlace();
                win->Unlock();
            }
            return done;
        }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).name == "Verse"; }));
        // The zoom the sections above were written at (10b's click needed the
        // clip wide enough to hold the field).
        restoreZoom(zoomWas);
    }

    // --- 11. Fade shapes (M2.5) -------------------------------------------
    std::printf("  depth: fade shapes\n");
    {
        // A fade the clip owns, then each shape through the menu's own method.
        if (win->LockWithTimeout(1000000) == B_OK) {
            stack->Execute(std::make_unique<SetClipFadeCommand>(
                               ta.id, ac.id, 12000, 0), project);
            tv->SetClipFadeShapeAt(ta.id, ac.id, FadeShape::SCurve);
            win->Unlock();
        }
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).fadeShape
                                   == FadeShape::SCurve; }));
        Shot("depth-fade-scurve");
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetClipFadeShapeAt(ta.id, ac.id, FadeShape::EqualPower);
            win->Unlock();
        }
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).fadeShape
                                   == FadeShape::EqualPower; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).fadeShape
                                   == FadeShape::SCurve; }));
        // One more undo for the shape itself; the fade it belongs to is the
        // step after that.
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] {
            const Clip c = clipOf(ta.id, ac.id);
            return c.fadeShape == FadeShape::Linear && c.fadeInFrames == 12000;
        }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).fadeInFrames == 0; }));
    }

    // --- 12. Nudge with , and . by one grid step --------------------------
    std::printf("  depth: nudge\n");
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetSnapGrid({ SnapKind::Quarter, false });
            tv->SelectAllClips();
            win->Unlock();
        }
        Frame step = 0;
        {
            if (win->LockWithTimeout(1000000) == B_OK) {
                step = SnapStep(project.tempoMap, 48000,
                                { SnapKind::Quarter, false });
                win->Unlock();
            }
        }
        CHECK(step > 1);   // a real grid step, not a frame
        const Frame beforeA = clipOf(ta.id, ac.id).startFrame;
        sendKey(".");
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).startFrame == beforeA + step; }));
        CHECK(undoName() == "Nudge Right");
        sendKey(",");
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).startFrame == beforeA; }));
        sendKey(".");
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).startFrame == beforeA + step; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).startFrame == beforeA; }));
        if (win->LockWithTimeout(1000000) == B_OK) {
            tv->SetSnapGrid({ SnapKind::Sixteenth, false });
            win->Unlock();
        }
    }

    // --- 13. Slip: Alt-drag the audio inside the clip ---------------------
    std::printf("  depth: slip gesture\n");
    {
        // Deselect so the drag acts on the clip under the pointer alone.
        sendKey("\x1b");
        CHECK(WaitFor([&] {
            bool ok = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                ok = tv->SelectionCount() == 0;
                win->Unlock();
            }
            return ok;
        }));
        const Frame offBefore = clipOf(ta.id, ac.id).sourceOffset;
        const Frame startBefore = clipOf(ta.id, ac.id).startFrame;
        const float x = frameX(60000);
        postMouse(B_MOUSE_DOWN, BPoint(x, laneMidA), B_OPTION_KEY);
        postMouse(B_MOUSE_MOVED, BPoint(x + 20.0f, laneMidA), B_OPTION_KEY);
        postMouse(B_MOUSE_UP, BPoint(x + 20.0f, laneMidA), B_OPTION_KEY);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).sourceOffset > offBefore; }));
        const Clip moved = clipOf(ta.id, ac.id);
        CHECK(moved.startFrame == startBefore);      // the clip did not move
        CHECK(moved.lengthFrames == 48000);          // ... nor change length
        CHECK(undoName() == "Slip Clip");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).sourceOffset == offBefore; }));
        // One more nudge back so the engine's view of the fixture is unchanged.
    }

    // --- 14. Cmd-Z reaches the undo stack from a child window -------------
    std::printf("  depth: child window forwards Cmd-Z\n");
    {
        // A real edit to undo, made through the stack.
        {
            if (win->LockWithTimeout(1000000) == B_OK) {
                stack->Execute(std::make_unique<SetClipNameCommand>(
                                   ta.id, ac.id, "Child Undo"), project);
                win->Unlock();
            }
        }
        CHECK(clipOf(ta.id, ac.id).name == "Child Undo");

        // Open the (single-instance) effects window — a child window that
        // forwards Space, and now Cmd-Z — and send it a Cmd-Z key the way the
        // input server delivers one.
        const int before = VisibleWindows();
        {
            BMessage show(kMsgShowFx);
            show.AddInt64("track", (int64)ta.id);
            show.AddInt32("focus", -1);
            win->PostMessage(&show);
        }
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        BWindow* child = nullptr;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden() && w != win) child = w;
        }
        CHECK(child != nullptr);
        if (child) {
            std::printf("  child window: '%s'\n", WindowTitle(child));
            BMessage key(B_KEY_DOWN);
            key.AddString("bytes", "z");
            key.AddInt32("modifiers", B_COMMAND_KEY);
            BMessenger(child).SendMessage(&key);
            CHECK(WaitFor([&] { return clipOf(ta.id, ac.id).name != "Child Undo"; }));
        }
        // Close the child again. Lock, Quit(), and NO Unlock: Quit tears the
        // looper down, and touching it afterwards trips "looper must be locked
        // before proceeding" — a crash dialog on the target, not a failed
        // check (the pattern every other close in this file follows).
        if (child && child->LockWithTimeout(2000000) == B_OK) child->Quit();
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
    }

    // --- 15. A header fader/pan drag is live in the OTHER panes too ---------
    // The mixer/inspector -> timeline direction has always been live; the
    // reverse was not: the header drag previewed into the model and refreshed
    // the panes only on mouse-up, so the inspector and the mixer showed the old
    // value until the gesture ended. This asserts the value MID-DRAG, before
    // the mouse-up, on the mixer's own snapshot of the strip (the observable
    // the inspector, which draws straight from the model, cannot offer).
    std::printf("  depth: header drag refreshes the panes mid-gesture\n");
    {
        win->PostMessage(MSG_MIXER_TEST);
        MixerStripsView* strips = nullptr;
        BWindow* mixer = nullptr;
        CHECK(WaitFor([&] {
            for (int32 i = 0; i < be_app->CountWindows(); i++) {
                BWindow* w = be_app->WindowAt(i);
                if (!w || w->IsHidden() || w == win) continue;
                if (w->LockWithTimeout(500000) != B_OK) continue;
                BView* v = w->FindView("strips");
                if (v) { mixer = w; strips = dynamic_cast<MixerStripsView*>(v); }
                w->Unlock();
                if (strips) return true;
            }
            return false;
        }));
        CHECK(strips != nullptr);
        if (strips) {
            auto mixerGain = [&] {
                float g = -1.0f;
                if (mixer->LockWithTimeout(1000000) == B_OK) {
                    for (const MixerStripInfo& st : strips->Strips())
                        if (st.trackId == ta.id) g = st.gain;
                    mixer->Unlock();
                }
                return g;
            };
            const float beforeG = mixerGain();
            CHECK(beforeG >= 0.0f);

            // Press on the header fader and move. The button stays DOWN while
            // the mixer is polled: the refresh has to reach it during the
            // gesture, not on mouse-up (that is the whole point of the check,
            // and polling with the button held is what makes it a real one).
            // The row comes from the view's own geometry: a hard-coded offset
            // silently pressed the WRONG control when M2.6 moved the fader --
            // and the control under it (the input picker) opens a synchronous
            // popup, which wedged the whole run.
            BRect gainR;
            if (win->LockWithTimeout(1000000) == B_OK) {
                gainR = tv->HeaderRectFor(laneIdx, TimelineView::HeaderControl::Gain);
                win->Unlock();
            }
            const float gy = (gainR.top + gainR.bottom) * 0.5f;
            CHECK(gainR.Height() > 1.0f);
            postMouse(B_MOUSE_DOWN, BPoint(Themed(26.0f), gy));
            postMouse(B_MOUSE_MOVED, BPoint(Themed(146.0f), gy));
            const bool midLive = WaitFor(
                [&] { return mixerGain() > beforeG + 0.01f; }, 5000000);
            std::printf("  header drag: mixer gain %.3f -> %.3f (mid-drag: %s)\n",
                        beforeG, mixerGain(), midLive ? "yes" : "no");
            CHECK(midLive);                    // still mid-drag: the button is down
            postMouse(B_MOUSE_UP, BPoint(Themed(146.0f), gy));
            CHECK(WaitFor([&] { return mixerGain() > beforeG + 0.01f; }));
        }
        if (mixer && mixer->LockWithTimeout(2000000) == B_OK) mixer->Quit();
        CHECK(WaitFor([&] { return VisibleWindows() == 1; }));
    }

    // Leave the window as the next test (the big-project one) expects: pointer
    // tool, no range, no selection.
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv->SetTool(TimelineView::Tool::Pointer);
        tv->ClearTimeRange();
        win->Unlock();
    }
    sendKey("\x1b");
    snooze(150000);
}

// --- M2.6/M2.7: track depth and markers -------------------------------------
//
// Every check here drives the REAL widget: a synthetic mouse on the view, the
// keystrokes the window routes to it, and the message the real menu item
// posts. The one thing that cannot be driven is a popup menu (its Go() is
// synchronous and would wedge the window thread), so the pickers' CHOICES run
// through the method the menu item calls -- and what the popup looks like is
// what the shots are for. A `HeaderControlAt` assertion on the control's own
// rect proves the press would have landed there.
static void TestArrangeTracks(MainWindow* win, Project& project,
                              CommandStack* stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_arrange_tracks\n");

    // Fixture: two audio tracks (each with one clip) at the end of the list.
    const char* kWav = "/tmp/haiku_daw_ui_tracks.wav";
    {
        std::remove(kWav);
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 2500, -2500 };
        CHECK(w.Open(kWav, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
    }
    // What the project looked like before this test (section 10 puts it back).
    std::vector<Marker> markersWere;
    Frame playheadWas = 0;
    size_t tracksWere = 0;
    if (win->LockWithTimeout(1000000) == B_OK) {
        markersWere = project.markers;
        playheadWas = project.transport.playhead;
        tracksWere  = project.Tracks().size();
        win->Unlock();
    }
    auto makeTrack = [&](const char* name, Frame start) {
        Track t;
        t.id = project.NextTrackId();
        t.type = TrackType::Audio;
        t.name = name;
        Clip c;
        c.id = project.NextClipId();
        c.startFrame = start;
        c.lengthFrames = 24000;
        c.sourcePath = kWav;
        t.clips.push_back(c);
        CHECK(LockedAddTrack(win, project, t));
        return t;
    };
    const Track ta = makeTrack("td-a", 0);
    const Track tb = makeTrack("td-b", 0);
    const ClipId clipB = tb.clips[0].id;

    TimelineView* tv = nullptr;
    int idxA = -1, idxB = -1;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tv = dynamic_cast<TimelineView*>(win->FindView("timeline"));
        idxA = project.IndexOfTrack(ta.id);
        idxB = project.IndexOfTrack(tb.id);
        win->Unlock();
    }
    CHECK(tv != nullptr);
    if (!tv) return;

    auto lock = [&](const std::function<void()>& fn) {
        if (win->LockWithTimeout(1000000) == B_OK) { fn(); win->Unlock(); }
    };
    // Post a mouse event to any view with a point in ITS OWN coordinates: the
    // view's window is locked for the conversion (the app server delivers the
    // press as ConvertFromScreen(screen_where), so the pair has to agree), and
    // the view's window -- not the main window -- is the one locked, so this
    // works for a button in the markers window too.
    auto postMouseTo = [&](BView* view, uint32 what, BPoint at, uint32 mods,
                           int32 buttons, int32 clicks) {
        BPoint screen = at;
        if (BWindow* vw = view->Window()) {
            if (vw->LockWithTimeout(1000000) == B_OK) {
                screen = view->ConvertToScreen(at);
                vw->Unlock();
            }
        }
        BMessage m(what);
        m.AddInt32("buttons", buttons);
        m.AddInt32("clicks", clicks);
        m.AddInt32("modifiers", (int32)mods);
        m.AddPoint("where", at);
        m.AddPoint("screen_where", screen);
        BMessenger(view).SendMessage(&m);
    };
    auto trackCount = [&] {
        size_t n = 0;
        lock([&] { n = project.Tracks().size(); });
        return n;
    };
    auto markerCount = [&] {
        size_t n = 0;
        lock([&] { n = project.markers.size(); });
        return n;
    };
    auto press = [&](BPoint at, uint32 mods = 0, int32 buttons = 1,
                     int32 clicks = 1) {
        postMouseTo(tv, B_MOUSE_DOWN, at, mods, buttons, clicks);
        postMouseTo(tv, B_MOUSE_UP, at, mods, 0, clicks);
    };
    auto sendKey = [&](const char* bytes, uint32 mods = 0) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", (int32)mods);
        win->PostMessage(&key);
    };
    auto trackName = [&](TrackId id) {
        std::string s;
        lock([&] {
            if (const Track* t = project.FindTrack(id)) s = t->name;
        });
        return s;
    };
    auto trackOf = [&](TrackId id) {
        Track t;
        lock([&] {
            if (const Track* p = project.FindTrack(id)) t = *p;
        });
        return t;
    };
    auto undoName = [&] {
        std::string s;
        lock([&] { s = stack->UndoName(); });
        return s;
    };
    auto laneAt = [&](int idx) {
        BRect r;
        lock([&] { r = tv->LaneFrame(idx); });
        return r;
    };
    auto controlCentre = [&](int idx, TimelineView::HeaderControl which) {
        BRect r;
        lock([&] { r = tv->HeaderRectFor(idx, which); });
        return BPoint((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
    };

    // Bring the two new lanes into view (they are the last ones).
    {
        BMessage bottomWheel(B_MOUSE_WHEEL_CHANGED);
        bottomWheel.AddFloat("be:wheel_delta_y", 100000.0f);
        BMessenger(tv).SendMessage(&bottomWheel);
        snooze(120000);
        lock([&] { tv->ScrollToFrame(0); });
        snooze(120000);
    }
    const BRect laneA = laneAt(idxA);
    const BRect laneB = laneAt(idxB);
    CHECK(laneA.top >= TimelineContentTop());
    CHECK(laneB.top > laneA.top);

    // --- 1. Rename a track in place (M2.6) --------------------------------
    std::printf("  tracks: rename in place\n");
    {
        const BPoint nameAt(Themed(40.0f), laneA.top + Themed(8.0f));
        press(nameAt, 0, 1, 2);            // double-click the name
        CHECK(WaitFor([&] {
            bool on = false;
            lock([&] { on = tv->RenamingInPlace() && tv->RenamingTrack() == ta.id; });
            return on;
        }));
        // The field owns the keys: four backspaces clear "td-a", then a name.
        for (int i = 0; i < 4; i++) sendKey("\x08");
        sendKey("L"); sendKey("e"); sendKey("a"); sendKey("d");
        snooze(250000);
        Shot("tracks-rename-field");       // the open field, caret and all
        sendKey("\n");                     // Enter commits
        CHECK(WaitFor([&] { return trackName(ta.id) == "Lead"; }));
        CHECK(undoName() == "Rename Track");
        CHECK(WaitFor([&] {
            bool done = false;
            lock([&] { done = !tv->RenamingInPlace(); });
            return done;
        }));
        Shot("tracks-rename");             // the open field went away with Enter
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return trackName(ta.id) == "td-a"; }));

        // Esc leaves the name as it was.
        press(nameAt, 0, 1, 2);
        CHECK(WaitFor([&] {
            bool on = false;
            lock([&] { on = tv->RenamingInPlace(); });
            return on;
        }));
        sendKey("X");
        sendKey("\x1b");                   // Esc cancels
        CHECK(WaitFor([&] {
            bool done = false;
            lock([&] { done = !tv->RenamingInPlace(); });
            return done;
        }));
        CHECK(trackName(ta.id) == "td-a");
    }

    // --- 2. Drag the lane edge to resize it (M2.6) ------------------------
    std::printf("  tracks: lane-edge resize\n");
    {
        const float edgeY = laneAt(idxB).bottom;
        const BPoint grab(Themed(40.0f), edgeY - Themed(1.0f));
        // The widget's own hit test names this lane's edge...
        int edgeIdx = -1;
        lock([&] { edgeIdx = tv->LaneEdgeAt(grab); });
        CHECK(edgeIdx == idxB);
        const int before = trackOf(tb.id).height;
        postMouseTo(tv, B_MOUSE_DOWN, grab, 0, 1, 1);
        postMouseTo(tv, B_MOUSE_MOVED,
                    BPoint(Themed(40.0f), edgeY + Themed(30.0f)), 0, 1, 1);
        postMouseTo(tv, B_MOUSE_UP,
                    BPoint(Themed(40.0f), edgeY + Themed(30.0f)), 0, 0, 1);
        CHECK(WaitFor([&] { return trackOf(tb.id).height > before + 20; }));
        CHECK(undoName() == "Set Track Height");
        Shot("tracks-lane-height");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return trackOf(tb.id).height == before; }));
    }

    // --- 3. The colour swatch draws and its picker's action works (M2.6) ---
    std::printf("  tracks: colour swatch\n");
    {
        const BPoint sw = controlCentre(idxB, TimelineView::HeaderControl::Colour);
        TimelineView::HeaderControl hit = TimelineView::HeaderControl::None;
        lock([&] { hit = tv->HeaderControlAt(idxB, sw); });
        CHECK(hit == TimelineView::HeaderControl::Colour);   // the real geometry
        lock([&] { tv->SetTrackColour(tb.id, 3); });         // the menu's own call
        CHECK(WaitFor([&] { return trackOf(tb.id).colorIndex == 3; }));
        CHECK(undoName() == "Set Track Color");
        Shot("tracks-colour");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return trackOf(tb.id).colorIndex == 0; }));
    }

    // --- 4. The header's input selector (M2.6) -----------------------------
    std::printf("  tracks: input selector\n");
    {
        const BPoint in = controlCentre(idxB, TimelineView::HeaderControl::Input);
        TimelineView::HeaderControl hit = TimelineView::HeaderControl::None;
        lock([&] { hit = tv->HeaderControlAt(idxB, in); });
        CHECK(hit == TimelineView::HeaderControl::Input);
        // The action the popup entry runs (a MIDI track would offer endpoints;
        // an audio track offers the system default -- TrackInputs.h).
        lock([&] { tv->SetTrackInput(tb.id, InputSource{ InputSource::kAudioDefault, "", 0 }); });
        CHECK(WaitFor([&] {
            return trackOf(tb.id).input.kind == InputSource::kAudioDefault;
        }));
        CHECK(undoName() == "Set Track Input");
        Shot("tracks-input");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] {
            return trackOf(tb.id).input.kind == InputSource::kNone;
        }));
    }

    // --- 5. Fold the lane (M2.6) -------------------------------------------
    std::printf("  tracks: collapse\n");
    {
        const BPoint fold = controlCentre(idxB, TimelineView::HeaderControl::Fold);
        TimelineView::HeaderControl hit = TimelineView::HeaderControl::None;
        lock([&] { hit = tv->HeaderControlAt(idxB, fold); });
        CHECK(hit == TimelineView::HeaderControl::Fold);
        press(fold);                       // a real click on the triangle
        CHECK(WaitFor([&] { return trackOf(tb.id).collapsed; }));
        CHECK(undoName() == "Collapse Track");
        // The lane really folded: its drawn height is the title strip.
        const BRect folded = laneAt(idxB);
        CHECK(folded.Height() < TrackHeight());
        CHECK(folded.Height() > 4.0f);
        Shot("tracks-collapsed");
        // Re-ask the widget where the triangle is: collapsing the last lane
        // shrinks the content, the vertical scrollbar clamps (scroll-past-the-
        // end stays impossible), and every lane shifts down with it. A stale
        // point would press whatever moved under it.
        press(controlCentre(idxB, TimelineView::HeaderControl::Fold));
        CHECK(WaitFor([&] { return !trackOf(tb.id).collapsed; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return trackOf(tb.id).collapsed; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return !trackOf(tb.id).collapsed; }));
    }

    // --- 6. Alt-click solos one track exclusively (M2.6) -------------------
    std::printf("  tracks: exclusive solo\n");
    {
        const BPoint soloA = controlCentre(idxA, TimelineView::HeaderControl::Solo);
        const BPoint soloB = controlCentre(idxB, TimelineView::HeaderControl::Solo);
        press(soloA);                      // A solos normally first
        CHECK(WaitFor([&] { return trackOf(ta.id).soloed; }));
        press(soloB, B_OPTION_KEY);        // Alt-click B: B alone
        CHECK(WaitFor([&] { return trackOf(tb.id).soloed; }));
        CHECK(!trackOf(ta.id).soloed);
        CHECK(undoName() == "Exclusive Solo");
        Shot("tracks-exclusive-solo");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return !trackOf(tb.id).soloed && trackOf(ta.id).soloed; }));
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return !trackOf(ta.id).soloed; }));
    }

    // --- 7. Duplicate Track, from the Track menu's own message (M2.6) ------
    std::printf("  tracks: duplicate\n");
    {
        // Select B by clicking its header, so the menu item acts on it.
        press(BPoint(Themed(60.0f), laneAt(idxB).top + Themed(8.0f)));
        CHECK(WaitFor([&] {
            bool sel = false;
            lock([&] { sel = tv->SelectedTrack() == tb.id; });
            return sel;
        }));
        const size_t before = trackCount();
        win->PostMessage(MSG_DUPLICATE_TRACK);     // the Track menu's message
        CHECK(WaitFor([&] { return trackCount() == before + 1; }));
        CHECK(undoName() == "Duplicate Track");
        TrackId copyId = kInvalidTrackId;
        lock([&] {
            const int at = project.IndexOfTrack(tb.id);
            if (at >= 0 && at + 1 < (int)project.Tracks().size())
                copyId = project.Tracks()[(size_t)(at + 1)].id;
        });
        CHECK(copyId != kInvalidTrackId);
        const Track copy = trackOf(copyId);
        CHECK(copy.name == "td-b copy");
        CHECK(copy.clips.size() == 1);
        if (!copy.clips.empty()) {
            CHECK(copy.clips[0].id != clipB);      // fresh ids
            CHECK(copy.clips[0].sourcePath == kWav);
        }
        CHECK(WaitFor([&] {
            bool sel = false;
            lock([&] { sel = tv->SelectedTrack() == copyId; });
            return sel;
        }));
        Shot("tracks-duplicated");
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] { return trackCount() == before; }));
    }

    // --- 8. The marker lane (M2.7) -----------------------------------------
    std::printf("  markers: lane\n");
    {
        const size_t markersBefore = markerCount();
        win->PostMessage(MSG_MARKER_LANE);      // View > Marker Lane's message
        CHECK(WaitFor([&] {
            bool on = false;
            lock([&] { on = tv->MarkerLaneOn(); });
            return on;
        }));
        // The lane sits between the ruler and the lanes.
        float laneTop = 0.0f, rulerBottom = 0.0f;
        lock([&] { laneTop = tv->ContentTop(); rulerBottom = tv->RulerBottom(); });
        CHECK(laneTop > rulerBottom);
        const float yMarker = (laneTop + rulerBottom) * 0.5f;

        // Double-click empty marker-lane space: a marker appears there. The x
        // comes from the view's own frame mapping, under its lock.
        float xAt = 0.0f;
        lock([&] { xAt = tv->FrameToX(120000); });
        postMouseTo(tv, B_MOUSE_DOWN, BPoint(xAt, yMarker), 0, 1, 2);
        postMouseTo(tv, B_MOUSE_UP,   BPoint(xAt, yMarker), 0, 0, 2);
        CHECK(WaitFor([&] { return markerCount() == markersBefore + 1; }));
        Frame mkFrame = -1;
        lock([&] {
            if (!project.markers.empty()) mkFrame = project.markers.back().frame;
        });
        CHECK(mkFrame == 120000);         // the 1/16 grid at 120 BPM/48k
        Shot("markers-lane");

        // A click on the flag seeks -- and moves the DRAWN line, not just the
        // model (the ruler's flag click used to leave the line behind).
        lock([&] { project.transport.playhead = 0; tv->SetPlayhead(0); });
        press(BPoint(xAt, yMarker));
        CHECK(WaitFor([&] {
            Frame ph = -1, drawn = -1;
            lock([&] { ph = project.transport.playhead; drawn = tv->Playhead(); });
            return ph == mkFrame && drawn == mkFrame;
        }));

        // Dragging the flag MOVES the marker (package 50, ux-timeline): the
        // release lands it snapped on the grid, and the whole gesture is ONE
        // undo step (Move Marker).
        {
            float xTo = 0.0f;
            lock([&] { xTo = tv->FrameToX(180000); });
            postMouseTo(tv, B_MOUSE_DOWN,  BPoint(xAt, yMarker), 0, 1, 1);
            postMouseTo(tv, B_MOUSE_MOVED, BPoint(xTo, yMarker), 0, 1, 1);
            postMouseTo(tv, B_MOUSE_UP,    BPoint(xTo, yMarker), 0, 0, 1);
            CHECK(WaitFor([&] {
                Frame f = -1;
                lock([&] {
                    if (!project.markers.empty())
                        f = project.markers.back().frame;
                });
                return f == 180000;
            }));
            CHECK(markerCount() == markersBefore + 1);   // moved, not copied
            CHECK(undoName() == "Move Marker");
            // A test earlier in the run saved a project, so the autosave
            // runner can leave a recovery file behind -- and a modal "Recover?"
            // in front of this window makes the shot useless (found by reading
            // the pass: the marker lane was behind one). Clear it first; the
            // suite deletes it at start-up for the same reason.
            {
                BPath settings;
                if (ProjectDocument::SettingsPath(settings) == B_OK) {
                    BPath recovery(settings);
                    if (recovery.Append("recovery.dawproj") == B_OK)
                        BEntry(recovery.Path()).Remove();
                }
            }
            Shot("markers-lane-moved");
            win->PostMessage(MSG_EDIT_UNDO);
            CHECK(WaitFor([&] {
                Frame f = -1;
                lock([&] {
                    if (!project.markers.empty())
                        f = project.markers.back().frame;
                });
                return f == mkFrame;                     // back where it was
            }));
        }

        // The flag's right-click opens a menu now (Rename… / Delete) instead
        // of deleting the marker outright. A popup cannot be driven by the
        // suite -- and posting the click would sit in the popup's Go() -- so
        // the DELETE its menu entry runs is called directly: the established
        // pattern (a popup needs eyes; docs/UI_GUIDELINES.md §1).
        {
            std::string mkName;
            lock([&] {
                if (!project.markers.empty())
                    mkName = project.markers.back().name;
            });
            lock([&] { tv->RemoveMarkerAt(mkFrame, mkName); });
            CHECK(WaitFor([&] { return markerCount() == markersBefore; }));
            CHECK(undoName() == "Remove Marker");
            win->PostMessage(MSG_EDIT_UNDO);
            CHECK(WaitFor([&] { return markerCount() == markersBefore + 1; }));
        }

        // ...and the playhead-seek from a marker is what the markers window
        // uses too (checked in section 9).
        win->PostMessage(MSG_EDIT_UNDO);   // drop the marker again
        CHECK(WaitFor([&] { return markerCount() == markersBefore; }));

        // Add two markers for the window's list, then leave the lane OFF so
        // the later tests see the geometry they expect.
        {
            lock([&] { project.transport.playhead = 24000; });
            lock([&] { tv->AddMarkerAt(24000); });
            lock([&] { tv->AddMarkerAt(72000); });
        }
        CHECK(WaitFor([&] { return markerCount() == markersBefore + 2; }));
        win->PostMessage(MSG_MARKER_LANE);
        CHECK(WaitFor([&] {
            bool off = false;
            lock([&] { off = !tv->MarkerLaneOn(); });
            return off;
        }));
    }

    // --- 9. The markers window (M2.7) --------------------------------------
    std::printf("  markers: the list window\n");
    {
        const int before = VisibleWindows();
        win->PostMessage(MSG_MARKERS);        // View > Markers' own message
        BWindow* mw = nullptr;
        CHECK(WaitFor([&] {
            for (int32 i = 0; i < be_app->CountWindows(); i++) {
                BWindow* w = be_app->WindowAt(i);
                if (w && !w->IsHidden() && w != win
                    && std::strcmp(WindowTitle(w), "Markers") == 0) mw = w;
            }
            return mw != nullptr && VisibleWindows() == before + 1;
        }));
        CHECK(mw != nullptr);
        if (mw) {
            MarkersView* mv = nullptr;
            if (mw->LockWithTimeout(1000000) == B_OK) {
                mv = dynamic_cast<MarkersView*>(mw->FindView("markers"));
                mw->Unlock();
            }
            CHECK(mv != nullptr);
            // The list is the pushed snapshot, not model access.
            CHECK(WaitFor([&] {
                size_t n = 0;
                if (mw->LockWithTimeout(1000000) == B_OK) {
                    n = mv ? mv->Markers().size() : 0;
                    mw->Unlock();
                }
                return n == markerCount();
            }));
            // The list is kept CURRENT, not merely handed over at creation: a
            // marker added in the arranger while the window is open has to
            // appear here (that is the push, and without this check the
            // constructor's snapshot alone satisfies the size comparison).
            const size_t beforeAdd = markerCount();
            lock([&] { tv->AddMarkerAt(48000); });
            CHECK(WaitFor([&] { return markerCount() == beforeAdd + 1; }));
            CHECK(WaitFor([&] {
                size_t n = 0;
                if (mw->LockWithTimeout(1000000) == B_OK) {
                    n = mv ? mv->Markers().size() : 0;
                    mw->Unlock();
                }
                return n == beforeAdd + 1;
            }));
            snooze(200000);
            Shot("markers-window");

            // The real "Add at Playhead" button, clicked the way the server
            // delivers a click.
            if (mv) {
                BView* add = nullptr;
                BView* del = nullptr;
                if (mw->LockWithTimeout(1000000) == B_OK) {
                    add = mw->FindView("mk-add");
                    del = mw->FindView("mk-delete");
                    mw->Unlock();
                }
                CHECK(add != nullptr && del != nullptr);
                const size_t n0 = markerCount();
                if (add) {
                    // The button in ITS OWN coordinates, like TestWidgetKit.
                    BPoint at;
                    if (mw->LockWithTimeout(1000000) == B_OK) {
                        at = BPoint(add->Bounds().Width() * 0.5f,
                                    add->Bounds().Height() * 0.5f);
                        mw->Unlock();
                    }
                    postMouseTo(add, B_MOUSE_DOWN, at, 0, 1, 1);
                    postMouseTo(add, B_MOUSE_UP,   at, 0, 0, 1);
                    CHECK(WaitFor([&] { return markerCount() == n0 + 1; }));
                }
                // Clicking a row locates to that marker.
                Frame rowFrame = -1;
                if (mw->LockWithTimeout(1000000) == B_OK) {
                    if (mv && !mv->Markers().empty())
                        rowFrame = mv->Markers()[1].frame;
                    mw->Unlock();
                }
                CHECK(rowFrame >= 0);
                if (rowFrame >= 0) {
                    lock([&] { project.transport.playhead = 0; tv->SetPlayhead(0); });
                    postMouseTo(mv, B_MOUSE_DOWN,
                                BPoint(Themed(40.0f), Themed(16.0f)
                                       + Themed(2.0f) + Themed(22.0f) * 1.5f),
                                0, 1, 1);
                    postMouseTo(mv, B_MOUSE_UP,
                                BPoint(Themed(40.0f), Themed(16.0f)
                                       + Themed(2.0f) + Themed(22.0f) * 1.5f),
                                0, 0, 1);
                    CHECK(WaitFor([&] {
                        Frame ph = -1;
                        lock([&] { ph = project.transport.playhead; });
                        return ph == rowFrame;
                    }));
                }
                // ...and Delete removes the selected row.
                const size_t n1 = markerCount();
                if (del) {
                    BPoint at;
                    if (mw->LockWithTimeout(1000000) == B_OK) {
                        at = BPoint(del->Bounds().Width() * 0.5f,
                                    del->Bounds().Height() * 0.5f);
                        mw->Unlock();
                    }
                    postMouseTo(del, B_MOUSE_DOWN, at, 0, 1, 1);
                    postMouseTo(del, B_MOUSE_UP,   at, 0, 0, 1);
                    CHECK(WaitFor([&] { return markerCount() == n1 - 1; }));
                }
                Shot("markers-window-after");
            }

            // One instance: asking again brings the same window forward.
            win->PostMessage(MSG_MARKERS);
            snooze(300000);
            int titled = 0;
            for (int32 i = 0; i < be_app->CountWindows(); i++) {
                BWindow* w = be_app->WindowAt(i);
                if (w && !w->IsHidden()
                    && std::strcmp(WindowTitle(w), "Markers") == 0) titled++;
            }
            CHECK(titled == 1);
            // Close it the way every other child window is closed here.
            if (mw->LockWithTimeout(2000000) == B_OK) mw->Quit();
            CHECK(WaitFor([&] { return VisibleWindows() == before; }));
        }
    }

    // --- 10. Leave the project as it was found ----------------------------
    // The next test measures play start on the accumulated project against a
    // 5 s bound that is already marginal (the real budget is M4.1's), so this
    // package's fixtures must not stay behind and quietly tip it over. The two
    // fixture tracks go back through the stack and the marker list goes back
    // to exactly what it was (this is cleanup, not a behaviour under test).
    {
        lock([&] {
            stack->Execute(std::make_unique<RemoveTrackCommand>(ta.id), project);
            stack->Execute(std::make_unique<RemoveTrackCommand>(tb.id), project);
            project.markers = markersWere;
            project.transport.playhead = playheadWas;
            tv->SetTool(TimelineView::Tool::Pointer);
            tv->ClearTimeRange();
        });
        win->PostMessage(kMsgUiRefresh);
        CHECK(trackCount() == tracksWere);
        CHECK(markerCount() == markersWere.size());
    }
    sendKey("\x1b");
    snooze(150000);
}

// M1.3: the kit's controls are real BControls. A click lands on them the way
// the server delivers one (see TestThemeScale for why a synthetic click needs
// "screen_where"), a DawButton delivers its message, and a DawToggle reports
// the new value it flipped to.
namespace {

constexpr uint32 kMsgKitButton = 'kbt1';
constexpr uint32 kMsgKitToggle = 'ktg1';
constexpr uint32 kMsgKitCheck  = 'kck1';
constexpr uint32 kMsgKitSlider = 'ksl1';
constexpr uint32 kMsgKitKnob   = 'knb1';

class KitProbeWindow : public BWindow {
public:
    KitProbeWindow()
        : BWindow(BRect(300, 300, 560, 460), "kit-probe", B_TITLED_WINDOW_LOOK,
                  B_NORMAL_WINDOW_FEEL, B_AVOID_FOCUS) {}

    void MessageReceived(BMessage* msg) override {
        if (msg->what == kMsgKitButton || msg->what == kMsgKitToggle
            || msg->what == kMsgKitCheck || msg->what == kMsgKitSlider
            || msg->what == kMsgKitKnob) {
            fLast = msg->what;
            int32 v = -1;
            msg->FindInt32("be:value", &v);
            fValue = v;
            msg->FindFloat("value", &fFloat);
            return;   // consumed: the probe only observes
        }
        BWindow::MessageReceived(msg);
    }

    uint32 fLast  = 0;
    int32  fValue = -1;
    float  fFloat = -1.0f;   // the kit's own "value" on knobs
};

} // namespace

static void TestWidgetKit(MainWindow* win) {
    std::printf("widget kit ...\n");
    (void)win;

    KitProbeWindow* probe = new KitProbeWindow();
    BView* root = new BView(probe->Bounds(), "root", B_FOLLOW_ALL, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    probe->AddChild(root);
    DawButton* button = new DawButton(BRect(10, 10, 100, 32), "kit-btn",
                                      "Press", new BMessage(kMsgKitButton));
    DawToggle* toggle = new DawToggle(BRect(10, 40, 100, 62), "kit-tog",
                                      "Latch", new BMessage(kMsgKitToggle));
    DawCheckBox* check = new DawCheckBox(BRect(10, 70, 170, 90), "kit-chk",
                                         "Tick", new BMessage(kMsgKitCheck));
    DawSlider* slider = new DawSlider(BRect(10, 100, 240, 130), "kit-sld",
                                      "Level", new BMessage(kMsgKitSlider),
                                      0, 100);
    DawKnob* knob = new DawKnob(BRect(180, 5, 255, 95), "kit-knob", "Gain",
                                new BMessage(kMsgKitKnob), 0.0f, 100.0f);
    root->AddChild(button);
    root->AddChild(toggle);
    root->AddChild(check);
    root->AddChild(slider);
    root->AddChild(knob);
    // The probe window is a window like any other: what shows behind its root
    // view is the window's own background — the system panel colour — so it
    // gets the theme's, exactly as the app's windows do through ThemeAware.
    // Without this a dark run has a light rim around a dark panel, which is
    // the thing the pass is supposed to catch, not produce.
    probe->Show();
    if (probe->Lock()) {
        root->ResizeTo(probe->Bounds().Width(), probe->Bounds().Height());
        probe->Unlock();
    }
    ThemeStockTopView(probe);
    snooze(250000);
    Shot("widget-kit");

    auto postMouse = [&](BView* view, uint32 what, BPoint at, int32 clicks) {
        BPoint screen(at);
        if (probe->LockWithTimeout(1000000) == B_OK) {
            screen = view->ConvertToScreen(at);
            probe->Unlock();
        }
        BMessage m(what);
        m.AddInt32("buttons", what == B_MOUSE_UP ? 0 : 1);
        m.AddInt32("clicks", clicks);
        m.AddPoint("where", at);
        m.AddPoint("screen_where", screen);
        BMessenger(view).SendMessage(&m);
    };
    auto click = [&](BView* view, BPoint at) {
        postMouse(view, B_MOUSE_DOWN, at, 1);
        postMouse(view, B_MOUSE_UP, at, 1);
    };
    auto drag = [&](BView* view, BPoint from, BPoint to) {
        postMouse(view, B_MOUSE_DOWN, from, 1);
        postMouse(view, B_MOUSE_MOVED, to, 1);
        postMouse(view, B_MOUSE_UP, to, 1);
    };
    auto probeState = [&](uint32 what, int32 value) {
        return WaitFor([&] {
            if (probe->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = probe->fLast == what && probe->fValue == value;
            probe->Unlock();
            return ok;
        }, 5000000);
    };

    // A push button keeps its value at off; the point is that its message
    // arrives here at all.
    click(button, BPoint(45, 11));   // the control's own coordinates
    CHECK(probeState(kMsgKitButton, B_CONTROL_OFF));

    // The toggle reports the value it flipped TO: off -> on -> off.
    click(toggle, BPoint(45, 11));
    CHECK(probeState(kMsgKitToggle, B_CONTROL_ON));
    // ... and it still HOLDS it: the control's own state is the thing a lamp
    // is drawn from, so a toggle that reports ON and holds OFF is the bug in
    // waiting (BButton::Invoke resets the value to off after the invoke, which
    // is what made the second click below report ON again).
    {
        int32 held = -1;
        if (probe->LockWithTimeout(1000000) == B_OK) {
            held = toggle->Value();
            probe->Unlock();
        }
        CHECK(held == B_CONTROL_ON);
    }
    click(toggle, BPoint(45, 11));
    CHECK(probeState(kMsgKitToggle, B_CONTROL_OFF));

    // The tick box: the click flips it and the message carries the new value.
    click(check, BPoint(40, 10));
    CHECK(probeState(kMsgKitCheck, B_CONTROL_ON));
    click(check, BPoint(40, 10));
    CHECK(probeState(kMsgKitCheck, B_CONTROL_OFF));

    // The slider: a click near its right end moves the value there. The value
    // is read back from the control as well as off the message, so a control
    // that reports one thing and holds another is caught.
    click(slider, BPoint(220, 15));
    CHECK(WaitFor([&] {
        if (probe->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = probe->fLast == kMsgKitSlider && probe->fValue > 50;
        probe->Unlock();
        return ok;
    }, 5000000));
    int32 sliderValue = -1;
    if (probe->LockWithTimeout(1000000) == B_OK) {
        if (BSlider* sl = dynamic_cast<BSlider*>(probe->FindView("kit-sld")))
            sliderValue = sl->Value();
        probe->Unlock();
    }
    CHECK(sliderValue > 50);

    // The knob: a vertical drag up raises the value, and the message carries
    // it as a float as well as BControl's own be:value.
    drag(knob, BPoint(37, 45), BPoint(37, 15));
    CHECK(WaitFor([&] {
        if (probe->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = probe->fLast == kMsgKitKnob && probe->fFloat > 10.0f;
        probe->Unlock();
        return ok;
    }, 5000000));
    float knobValue = -1.0f;
    if (probe->LockWithTimeout(1000000) == B_OK) {
        if (DawKnob* k = dynamic_cast<DawKnob*>(probe->FindView("kit-knob")))
            knobValue = k->FloatValue();
        probe->Unlock();
    }
    CHECK(knobValue > 10.0f);
    Shot("widget-kit-used");

    // Pressed, then released outside the control: no invocation. The point is
    // outside the button but inside the window, so the message is dispatched.
    if (probe->LockWithTimeout(1000000) == B_OK) {
        probe->fLast = 0;
        probe->Unlock();
    }
    click(button, BPoint(55, 80));
    snooze(200000);
    bool invokedOutside = true;
    if (probe->LockWithTimeout(1000000) == B_OK) {
        invokedOutside = probe->fLast != 0;
        probe->Unlock();
    }
    CHECK(!invokedOutside);

    // --- the default button and Tab (the UX-shell pass) ---------------------
    // A kit button IS a BButton now: that is the type BWindow::SetDefaultButton
    // takes and calls back into, so MakeDefault can register instead of only
    // drawing a border (which is what it did: Return did nothing in every
    // converted dialog).
    CHECK(dynamic_cast<BButton*>(button) != nullptr);
    // Flags() on a BView is a locked call: BView::Flags() runs _CheckLock(),
    // which trips the debugger ("Looper must be locked.") when the view's
    // window lock is not held by the calling thread -- and a faulted run
    // wedges the whole team (docs/HANDOFF.md). Read them under the probe's
    // lock, like every other view call here.
    bool tabReachable = false;
    if (probe->LockWithTimeout(1000000) == B_OK) {
        tabReachable = (button->Flags() & B_NAVIGABLE) != 0
                    && (toggle->Flags() & B_NAVIGABLE) != 0;
        probe->Unlock();
    }
    CHECK(tabReachable);

    // MakeDefault reaches the WINDOW: that is the registration BWindow routes
    // Return to (BWindow::_DetermineTarget), and the thing that was missing.
    if (probe->LockWithTimeout(1000000) == B_OK) {
        button->MakeDefault(true);
        probe->Unlock();
    }
    CHECK(WaitFor([&] {
        if (probe->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = probe->DefaultButton() == button;
        probe->Unlock();
        return ok;
    }, 5000000));
    // A default button DRAWS differently (the look's B_DEFAULT_BUTTON frame),
    // which is what the shot below is for.
    Shot("widget-kit-default");

    // One default per window: making another button the default demotes this
    // one (BWindow's own bookkeeping, through the same override).
    if (probe->LockWithTimeout(1000000) == B_OK) {
        toggle->MakeDefault(true);
        probe->Unlock();
    }
    CHECK(WaitFor([&] {
        if (probe->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = probe->DefaultButton() == toggle;
        probe->Unlock();
        return ok;
    }, 5000000));
    if (probe->LockWithTimeout(1000000) == B_OK) {
        probe->SetDefaultButton(nullptr);   // no dangling default on teardown
        probe->Unlock();
    }

    probe->Lock();
    probe->Quit();
    snooze(150000);
}

// The three transient dialogs no other test opens get looked at here: Rename
// (the Save-Preset prompt's class), Quantize (the roll's) and the stems export
// dialog. Each is closed the way Escape closes it -- B_QUIT_REQUESTED -- and
// must go quietly, without sending anything.
static void TestDialogVocabulary(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("ux shell: dialog vocabulary ...\n");

    // Rename. A track id that exists (nothing is applied: the window is never
    // told OK).
    {
        const TrackId tid = project.Tracks().empty()
                          ? kInvalidTrackId : project.Tracks().front().id;
        const int before = VisibleWindows();
        RenameWindow* rn = new RenameWindow(BRect(200, 200, 460, 244), tid,
                                            "track-name", BMessenger(win), 0,
                                            0, nullptr);
        rn->Show();
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(250000);
        Shot("rename-dialog");
        CheckVocabulary(rn, "Rename Track", "ok", "OK");
        rn->PostMessage(B_QUIT_REQUESTED);
        CHECK(WaitFor([&] { return !WindowListed(rn); }));
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
    }

    // Quantize.
    {
        const int before = VisibleWindows();
        QuantizeOpts opts;
        QuantizeWindow* qz = new QuantizeWindow(BRect(200, 200, 500, 452),
                                                opts, BMessenger(win));
        qz->Show();
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(250000);
        Shot("quantize-dialog");
        CheckVocabulary(qz, "Quantize", "ok", "Quantize");
        qz->PostMessage(B_QUIT_REQUESTED);
        CHECK(WaitFor([&] { return !WindowListed(qz); }));
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
    }

    // The export options dialog, stems mode: its TITLE names the action (the
    // file panel behind it does), where "Export" alone did not say which.
    {
        const int before = VisibleWindows();
        ExportChoices c;
        ExportWindow* ew = new ExportWindow(BRect(200, 200, 580, 560), c,
                                            /*stems*/ true, BMessenger(win));
        ew->Show();
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(250000);
        Shot("export-stems-dialog");
        CheckVocabulary(ew, "Export Stems", "ok", "Export Stems");
        ew->PostMessage(B_QUIT_REQUESTED);
        CHECK(WaitFor([&] { return !WindowListed(ew); }));
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
    }

    // The mixdown one carries the other title and verb, so the two cannot be
    // the same dialog under two names.
    {
        const int before = VisibleWindows();
        ExportChoices c;
        ExportWindow* ew = new ExportWindow(BRect(200, 200, 580, 560), c,
                                            /*stems*/ false, BMessenger(win));
        ew->Show();
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(200000);
        CheckVocabulary(ew, "Export Mix", "ok", "Export Mix");
        ew->PostMessage(B_QUIT_REQUESTED);
        CHECK(WaitFor([&] { return !WindowListed(ew); }));
    }

    CHECK(WaitQuiet());
}

// The status line's job messages (the UX-shell pass): a refused second render
// says so instead of returning in silence, a cancelled one says what happened,
// and either way the message is TRANSIENT -- after a few seconds the transport
// state takes the row back, so a stale "Export cancelled" is not still there
// ten minutes later.
static void TestRenderFeedback(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("ux shell: render feedback ...\n");

    // The fixture has to be (a) everything the bounce can render and (b) long.
    // (a) is the part the first two versions got wrong, and the exporter's own
    // code is what proves it: by this point in the suite an earlier meter test
    // has SOLOED its fixture and muted every other track, and ExportWav
    // computes BOTH its project end and the material it walks from the AUDIBLE
    // tracks only (`anySolo` -> only soloed ones). So the bounce rendered that
    // one short clip -- tens of milliseconds -- and the progress bar it raised
    // was gone before the test could look. The solo goes, the rest stays muted,
    // and the fixture is the only audible thing.
    {
        win->Lock();
        for (Track& other : project.Tracks()) {
            other.soloed = false;   // a solo, not a mute, is what silenced us
            other.muted  = true;
        }
        win->Unlock();
        // A model change made directly posts the same refresh the app's own
        // edit paths do, or the screen shows the old lamps (UI_GUIDELINES §2).
        win->PostMessage(kMsgUiRefresh);
    }
    // (b) Twelve 120-second notes: seconds of real render (the twelve are a
    // chord -- the cost is per note-sample, not per second of material, which
    // is why one note was not enough), and the cancel below stops every one of
    // them within a chunk.
    for (int i = 0; i < 12; i++) {
        char name[32];
        std::snprintf(name, sizeof(name), "status-synth-%d", i);
        Track t = MakeMidiTrack(project, { { 60, 100, 0, 48000 * 120 } }, name);
        CHECK(LockedAddTrack(win, project, t));
    }
    // What the bounce will walk, in the log: the exporter's own rule (nothing
    // is soloed now, so audible = unmuted) over the same fields it reads. A
    // test that prints its fixture's size would have caught the empty-render
    // problem the first time.
    {
        Frame projEnd = 0;
        int audible = 0;
        win->Lock();
        for (const Track& tr : project.Tracks()) {
            if (tr.muted) continue;
            audible++;
            for (const Clip& c : tr.clips)
                if (c.startFrame + c.lengthFrames > projEnd)
                    projEnd = c.startFrame + c.lengthFrames;
            for (const MidiNote& n : tr.CollectPlaybackNotes())
                if (n.startFrame + n.lengthFrames > projEnd)
                    projEnd = n.startFrame + n.lengthFrames;
        }
        win->Unlock();
        std::printf("  fixture: %d audible track(s), %.0f s of material\n",
                    audible, (double)projEnd / project.sampleRate);
    }

    // The remembered choices are pinned first: by this point in the suite an
    // earlier test has left "Loop range" selected, and a loop-range bounce of
    // one second is not the fixture this test wants. Whole project, plain WAV.
    //
    // The pinning message ANSWERS WITH ITS FILE PANEL (that is how the export
    // options message works -- every export flow in this suite posts it and
    // then hides what it opens), so the panel is put away again before
    // anything here is COUNTED: a panel left up is one extra visible window,
    // and every count below (windowsBefore, the "exactly one bar" check,
    // WaitQuiet at the end) is about the quiet screen.
    {
        BMessage opts(kMsgExportOptions);
        opts.AddInt32("container", (int32)AudioFileFormat::Wav);
        opts.AddInt32("bits", 16);
        opts.AddInt32("range", 0);
        opts.AddInt32("stems", 0);
        win->PostMessage(&opts);
        MaybeHidePanels(win);   // the panel is up: hide it, and wait it out
    }
    CHECK(WaitQuiet());

    const char* pathA = "/tmp/haiku_daw_ui_status_a.wav";
    const char* pathB = "/tmp/haiku_daw_ui_status_b.wav";
    std::remove(pathA);
    std::remove(pathB);

    entry_ref dir;
    CHECK(BEntry("/tmp").GetRef(&dir) == B_OK);
    const int32 windowsBefore = VisibleWindows();
    BMessage a(MSG_EXPORT_REF);
    a.AddRef("directory", &dir);
    a.AddString("name", "haiku_daw_ui_status_a.wav");
    BMessage b(MSG_EXPORT_REF);
    b.AddRef("directory", &dir);
    b.AddString("name", "haiku_daw_ui_status_b.wav");

    // The second export is refused -- and SAYS so (it used to return in
    // silence, so choosing Export again looked like a dead menu item).

    // Both requests go in BACK TO BACK, and that is the guarantee: a window
    // handles its queue in order, so the first request has started its render
    // before the second is looked at, and no render of this fixture can be
    // over in the microseconds between two messages on one thread. The first
    // version waited for the first render's progress bar before asking for the
    // second -- a race the test can lose (a short bar is easy to miss between
    // polls, and a render that never rendered anything made it millisecond
    // short), and losing it turned "the second export is refused" into "the
    // second export ran".
    const bigtime_t askedUs = system_time();
    win->PostMessage(&a);
    win->PostMessage(&b);
    const bool refused = WaitFor([&] {
        return TransportStatus(win) == "A render is already running";
    }, 30000000);
    // A failure here has two very different shapes (the message never set, or
    // the message set and not read), and which one it is decides whether the
    // app or the test is at fault: print the row and the windows.
    if (!refused)
        std::printf("  DIAG no refusal: status \"%s\", windows %d (base %d)\n",
                    TransportStatus(win).c_str(), (int)VisibleWindows(),
                    (int)windowsBefore);
    CHECK(refused);
    // How long the request took to be handled is worth a line in the log: it
    // is the number a loaded VM moves, and it is what the first versions of
    // this test were really measuring.
    std::printf("  refusal after %.0f ms\n", (system_time() - askedUs) / 1000.0);
    // The refusal is only set while the first render IS running, so the bar it
    // raised is up right now -- and nothing was raised for the refused path.
    CHECK(VisibleWindows() == windowsBefore + 1);

    // Cancel the long one: the message on the row changes to the cancel.
    win->PostMessage(kMsgExportCancel);
    const bool cancelled = WaitFor([&] {
        return TransportStatus(win) == "Export cancelled";
    }, 60000000);
    if (!cancelled)
        std::printf("  DIAG no cancel message: status \"%s\", windows %d\n",
                    TransportStatus(win).c_str(), (int)VisibleWindows());
    CHECK(cancelled);
    CHECK(WaitFor([&] { return VisibleWindows() == windowsBefore; }, 60000000));
    CHECK(!FileExists(pathB));   // the refused render wrote nothing
    CHECK(WaitQuiet());

    // The hold is 4 s (kStatusHoldUs) and the deadline is checked by the 2 Hz
    // title poll, so the job message is gone inside a second of it.
    //
    // What comes back is the TRANSPORT'S STATE, and that is not always
    // "Stopped": with input monitoring live (a track left armed with its I
    // lamp, which this suite has by now) the state line is "Monitoring input",
    // and the row saying so is the app working, not the app failing (the run
    // that failed printed exactly that, and the message had never been set
    // because the refusal never happened -- see the fixture above). The
    // promise this check owns is the one the finding made: a job message does
    // not stand longer than its hold.
    const bool jobMessageGone = WaitFor([&] {
        const std::string s = TransportStatus(win);
        return s.find("Export") == std::string::npos
            && s != "A render is already running";
    }, 20000000);
    if (!jobMessageGone)
        std::printf("  DIAG the row after the hold: \"%s\"\n",
                    TransportStatus(win).c_str());
    CHECK(jobMessageGone);

    std::remove(pathA);
    std::remove(pathB);
}

// M1.4: the docked MIDI editor. The timeline posts kMsgOpenEditor (it no
// longer creates a window itself); the main window puts the editor in the
// bottom pane, uncollapses it, and "Pop out" hands the same region to a window
// of its own.
static void TestDockedEditor(MainWindow* win, Project& project,
                             CommandStack* stack) {
    std::printf("docked editor ...\n");
    Track t = MakeMidiTrack(project, { { 60, 100, 0, 4800 } }, "dock-roll");
    const TrackId tid = t.id;
    const ClipId  cid = t.midiClips.front().id;
    CHECK(LockedAddTrack(win, project, t));
    (void)stack;

    BMessage open(kMsgOpenEditor);
    open.AddInt64("track", (int64)tid);
    open.AddInt64("clip", (int64)cid);
    win->PostMessage(&open);

    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        BView* roll = win->FindView("roll");      // the docked PianoRollView
        BView* dock = win->FindView("dock");
        const bool ok = roll != nullptr && dock != nullptr
                     && dock->Bounds().Height() > Themed(20.0f);
        win->Unlock();
        return ok;
    }));
    Shot("docked-editor");

    // Pop out: the dock empties and a window of its own appears (and is closed
    // again, because closing the main window is what quits the app).
    // The inspector toggles the same way (View > Inspector, key I).
    // DIAGNOSTIC: the view's presence and the View > Inspector item's marked
    // state must agree before and after every toggle; the elapsed time says
    // whether the window thread was blocked instead.
    auto diagPanes = [&](const char* when) {
        bool view = false, marked = false;
        bigtime_t lockedUs = -1;
        const bigtime_t t0 = system_time();
        if (win->LockWithTimeout(1000000) == B_OK) {
            lockedUs = system_time() - t0;
            view = win->FindView("inspector") != nullptr;
            if (BMenuBar* mb = win->KeyMenuBar())
                for (int32 i = 0; i < mb->CountItems(); i++)
                    if (BMenuItem* it = mb->ItemAt(i)) {
                        const char* l = it->Label();
                        if (l && std::strcmp(l, "View") == 0 && it->Submenu()) {
                            BMenu* vm2 = it->Submenu();
                            for (int32 j = 0; j < vm2->CountItems(); j++)
                                if (BMenuItem* vi = vm2->ItemAt(j)) {
                                    const char* vl = vi->Label();
                                    if (vl && std::strcmp(vl, "Inspector") == 0)
                                        marked = vi->IsMarked();
                                }
                        }
                    }
            win->Unlock();
        }
        std::printf("  DIAG %s: view=%d marked=%d lock=%lldus\n", when, (int)view,
                    (int)marked, (long long)lockedUs);
    };
    diagPanes("before toggle 1");
    {
        const bigtime_t t0 = system_time();
        win->PostMessage(MSG_TOGGLE_INSPECTOR);
        snooze(200000);
        std::printf("  DIAG toggle 1 took %lldus wall\n",
                    (long long)(system_time() - t0));
    }
    diagPanes("after toggle 1");
    {
        bool inspGone = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            inspGone = win->FindView("inspector") == nullptr;
            win->Unlock();
        }
        CHECK(inspGone);
        Shot("inspector-hidden");
    }
    win->PostMessage(MSG_TOGGLE_INSPECTOR);
    snooze(200000);
    diagPanes("after toggle 2");
    {
        bool inspBack = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            inspBack = win->FindView("inspector") != nullptr;
            win->Unlock();
        }
        CHECK(inspBack);
    }

    const int before = VisibleWindows();
    win->PostMessage(MSG_POP_OUT_EDITOR);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        BView* roll = win->FindView("roll");
        BView* dock = win->FindView("dock");
        const bool ok = roll == nullptr && dock == nullptr;
        win->Unlock();
        return ok;
    }));
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
    Shot("popped-out-editor");

    // Close it again: it is the one visible window that is not the main one.
    // (Closing the main window is what quits the app, so it stays.)
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w == nullptr || w == win || w->IsHidden()) continue;
        if (w->LockWithTimeout(1000000) == B_OK) w->Quit();
        break;
    }
    CHECK(WaitFor([&] { return VisibleWindows() == before; }));
}

// Package 51: the roll's snapshot against the model — the stale-list bug and
// the region moving under an open editor — plus the grid the toolbar's field
// selects and the CC lane's point drag. All of it runs on the DOCKED editor,
// because that is where the main window's pushes and the Edit menu's routing
// live; the view's own looper does the message handling, so every touch of the
// view is under the window's lock (docs/HANDOFF.md: a view method off the
// window thread trips the kit's assertion).
static void TestRollModelSync(MainWindow* win, Project& project,
                              CommandStack* stack) {
    std::printf("piano roll: model sync, grid, lane drag (package 51)\n");
    // Two things this test learned on the target, and both are why every click
    // below is aimed with the VIEW's own mapping instead of with numbers that
    // only look right:
    //
    //  * the docked pane is SHORT (its split height persists; the default is
    //    Themed(260), so the roll's view is ~236 px tall). A pitch row 36
    //    semitones below the toolbar is far below the velocity lane, so a click
    //    meant for a note body lands IN THE LANE -- which is why the notes here
    //    sit high (90/94), and why every row is computed and checked;
    //  * the view centres itself on the playhead when one falls inside the
    //    region, so the transport is parked at 0 first and the scroll is printed
    //    and checked: a frame-derived x is only meaningful once it is known.
    // The notes sit on the TOP rows (92-95, plus the audition key at 96): the
    // usable band is [the toolbar, the velocity lane), and the pane's height is
    // persisted in the settings file -- the VM has had 200, 236 and 260 px
    // panes across runs. The top rows are inside the band for anything taller
    // than ~150 px, and the row CHECKs below say so with the height printed.
    Track t = MakeMidiTrack(project, { { 95, 100, 0, 4800 },
                                       { 94, 100, 6000, 3000 } }, "sync-roll");
    const TrackId tid = t.id;
    const ClipId  cid = t.midiClips.front().id;
    const Frame   origLen = t.midiClips.front().lengthFrames;
    CHECK(LockedAddTrack(win, project, t));

    {
        win->LockWithTimeout(1000000);
        project.transport.playhead = 0;
        win->Unlock();
    }

    BMessage open(kMsgOpenEditor);
    open.AddInt64("track", (int64)tid);
    open.AddInt64("clip", (int64)cid);
    win->PostMessage(&open);

    PianoRollView* roll = nullptr;
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        roll = dynamic_cast<PianoRollView*>(win->FindView("roll"));
        win->Unlock();
        return roll != nullptr;
    }));
    if (roll == nullptr) return;
    // Let the dock's split lay the pane out before its bounds are read: the
    // view exists as soon as it is a child, but its size arrives with the
    // layout, and every click below is aimed from this reading.
    snooze(300000);

    // The view's own terms: how big it is, where it looks, and where a pitch's
    // row falls. Every click below comes from these, so a click cannot miss the
    // pane without the aim CHECK saying so.
    float viewW = 0.0f, viewH = 0.0f;
    Frame scroll0 = 0;
    if (win->LockWithTimeout(1000000) == B_OK) {
        viewW = roll->Bounds().Width();
        viewH = roll->Bounds().Height();
        scroll0 = roll->ScrollFrame();
        win->Unlock();
    }
    std::printf("  roll view: %.0fx%.0f at scroll %lld\n", viewW, viewH,
                (long long)scroll0);
    const float laneTop = viewH - Themed(64.0f);
    // The y of a pitch's row, in the roll's own metrics (the toolbar's height
    // plus the semitone rows), half a row down so the click is mid-row.
    auto rowY = [&](int pitch) {
        return Themed(28.0f) + (float)(96 - pitch) * Themed(12.0f)
             + Themed(6.0f);
    };
    auto aimOk = [&](float x, float y) {
        const bool in = x >= Themed(58.0f) && x <= viewW
                     && y >= Themed(30.0f) && y < laneTop;
        if (!in)
            std::printf("  AIM MISS: (%.1f, %.1f) view %.0fx%.0f laneTop %.0f\n",
                        x, y, viewW, viewH, laneTop);
        return in;
    };
    CHECK(scroll0 == 0);                                        // the park worked
    CHECK(rowY(96) > Themed(30.0f) && rowY(96) < laneTop);   // the audition key
    CHECK(rowY(95) > Themed(30.0f) && rowY(95) < laneTop);   // the notes' rows
    CHECK(rowY(94) > Themed(30.0f) && rowY(94) < laneTop);
    CHECK(rowY(93) > Themed(30.0f) && rowY(93) < laneTop);   // the drawn ones
    CHECK(rowY(92) > Themed(30.0f) && rowY(92) < laneTop);

    // A key event for the window's FOCUS view. The token matters: a message
    // posted with the window's own token is not a "focus message"
    // (BWindow::_IsFocusMessage requires B_PREFERRED_TOKEN), so the menu's
    // shortcut path never runs and the key lands in BWindow::MessageReceived,
    // which does not handle keys -- the natural BMessenger(win) spelling sends
    // a key nowhere at all. A null handler gives the preferred token, whose
    // target the window keeps equal to the focus view.
    auto postKey = [&](const char* bytes, uint32 mods) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", mods);
        BMessenger((const BHandler*)nullptr, win).SendMessage(&key);
    };

    // A synthetic click, in VIEW coordinates. The message has to carry
    // screen_where: BWindow::DispatchMessage converts a mouse message's SCREEN
    // point into the view-local be:view_where that the kit's BView::
    // MessageReceived actually reads, and a message carrying only "where" is
    // not converted at all -- the view then sees (0, 0), the click lands in the
    // toolbar's corner, and every interaction in this test silently does
    // nothing. (The suite's older click helpers post screen_where for exactly
    // this reason; this test was written without it and paid for it.)
    auto click = [&](uint32 what, BPoint viewPt, uint32 mods, int32 buttons) {
        BPoint screen(viewPt);
        if (win->LockWithTimeout(1000000) == B_OK) {
            screen = roll->ConvertToScreen(viewPt);
            win->Unlock();
        }
        BMessage m(what);
        m.AddInt32("buttons", buttons);
        m.AddInt32("clicks", 1);
        m.AddInt32("modifiers", mods);
        m.AddPoint("where", viewPt);
        m.AddPoint("screen_where", screen);
        BMessenger(roll).SendMessage(&m);
    };

    // Reading the ROLL's own state, and the model's, under the window lock.
    auto rollNotes = [&]() -> std::vector<MidiNote> {
        std::vector<MidiNote> out;
        if (win->LockWithTimeout(1000000) != B_OK) return out;
        for (int i = 0; i < roll->NoteCount(); i++) out.push_back(roll->NoteForTest(i));
        win->Unlock();
        return out;
    };
    auto modelClip = [&](MidiClip* out) -> bool {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const Track* tr = project.FindTrack(tid);
        const MidiClip* c = tr ? tr->FindMidiClip(cid) : nullptr;
        if (c) *out = *c;
        win->Unlock();
        return c != nullptr;
    };

    // The docked editor is a view the main window HOSTS, so it opens synced:
    // that is what every later commit's base comes from.
    {
        bool synced = false;
        Frame start = -1, len = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            synced = roll->Synced();
            start  = roll->RegionStart();
            len    = roll->RegionLength();
            win->Unlock();
        }
        CHECK(synced);
        CHECK(start == 0);
        CHECK(len == origLen);
    }

    // The note the interactions grab: the WIDEST one, so its middle is far from
    // both of its edges whatever the zoom. Clicking near a note's head is a
    // different gesture (the left-edge resize) and a note one frame long cannot
    // be grabbed at all -- both are how this test's first cut turned its moves
    // into resizes and its toggles into new notes.
    auto widestNote = [&](int* idxOut) {
        MidiNote best{};
        int bestI = -1;
        Frame bestLen = -1;
        if (win->LockWithTimeout(1000000) != B_OK) return best;
        for (int i = 0; i < roll->NoteCount(); i++) {
            const MidiNote n = roll->NoteForTest(i);
            if (n.lengthFrames > bestLen) {
                bestLen = n.lengthFrames;
                best = n;
                bestI = i;
            }
        }
        win->Unlock();
        if (idxOut) *idxOut = bestI;
        return best;
    };

    // A click-drag on a note body MOVES it, and the commit lands in the model.
    // (The roll's own snapshot is what it commits, so the two agreeing after a
    // drag is not the point — the point is what happens on an undo below.)
    // Aimed at `idx`'s own row and at the MIDDLE of its body, moved by
    // `dFrames` worth of x: the middle is a move, the edges would be resizes.
    auto dragNote = [&](int idx, Frame dFrames) {
        float x = 0.0f, x2 = 0.0f, y = 0.0f;
        if (win->LockWithTimeout(1000000) != B_OK) return;
        if (idx < 0 || idx >= roll->NoteCount()) { win->Unlock(); return; }
        const MidiNote n = roll->NoteForTest(idx);
        const Frame mid = n.startFrame + std::max<Frame>(1, n.lengthFrames) / 2;
        x  = roll->XForFrameForTest(mid);
        x2 = roll->XForFrameForTest(mid + dFrames);
        y  = rowY(n.pitch);
        win->Unlock();
        CHECK(aimOk(x, y));
        CHECK(aimOk(x2, y));
        click(B_MOUSE_DOWN,  BPoint(x, y),  0, 1);
        click(B_MOUSE_MOVED, BPoint(x2, y), 0, 1);
        click(B_MOUSE_UP,    BPoint(x2, y), 0, 0);
        snooze(150000);
    };

    int grabIdx = -1;
    {
        const MidiNote wide = widestNote(&grabIdx);
        std::printf("  grabbing note %d (pitch %d, %lld frames)\n",
                    grabIdx, wide.pitch, (long long)wide.lengthFrames);
    }
    dragNote(grabIdx, 7000);   // ~one 16th to the right, snapped by the roll
    MidiClip now;
    CHECK(WaitFor([&] {
        MidiClip c;
        if (!modelClip(&c)) return false;
        now = c;
        return c.notes.size() == 2 && c.notes[0].startFrame != 0;
    }));
    if (now.notes.size() != 2 || now.notes[0].startFrame == 0)
        std::printf("  drag did not land: %d notes, first start %lld\n",
                    (int)now.notes.size(),
                    now.notes.empty() ? -1LL
                                      : (long long)now.notes[0].startFrame);

    // --- A. Undo: the model reverts, and the ROLL reverts with it -----------
    // Before package 51 the roll kept showing the move, and the next gesture
    // committed that stale list as a fresh edit — the model was silently
    // rewritten. The roll must now be re-read by the window's push.
    win->PostMessage(MSG_UNDO_TEST);
    CHECK(WaitFor([&] {
        MidiClip c;
        if (!modelClip(&c)) return false;
        if (c.notes[0].startFrame != 0) return false;
        const std::vector<MidiNote> shown = rollNotes();
        return shown.size() == 2 && shown[0].startFrame == 0;
    }));
    {
        const std::vector<MidiNote> shown = rollNotes();
        MidiClip c;
        modelClip(&c);
        CHECK(shown.size() == c.notes.size());
        CHECK(shown.size() == 2 && shown[0].startFrame == c.notes[0].startFrame);
    }
    Shot("roll-after-undo");

    // --- E. The grid the toolbar's field selects ----------------------------
    // Draw a note at a frame the TEST chooses (so both grids' answers are
    // known), then the same frame on the quarter grid: the start must be the
    // frame the kit-free snap says it is. The roll used to be welded to the
    // 16th, so triplet and swing parts could not be drawn at all.
    auto drawAt = [&](Frame f, int pitch) {
        float x = 0.0f;
        bool focused = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            x = roll->XForFrameForTest(f);
            focused = roll->IsFocus();
            win->Unlock();
        }
        // The drags above clicked the roll, so it holds the focus -- and the
        // Pencil tool is picked by posting the key to the WINDOW, the way a
        // keyboard sends it. The roll's editor keys are outside the arrange's
        // map, so this is also the check that the window hands them to the
        // docked editor rather than letting the map take them.
        CHECK(focused);
        postKey("2", 0);
        snooze(80000);
        const float y = rowY(pitch);
        CHECK(aimOk(x, y));
        click(B_MOUSE_DOWN, BPoint(x, y), 0, 1);
        click(B_MOUSE_UP,   BPoint(x, y), 0, 0);
        snooze(150000);
    };
    auto waitForNoteAt = [&](int pitch, Frame want) {
        return WaitFor([&] {
            MidiClip c;
            if (!modelClip(&c)) return false;
            for (const MidiNote& n : c.notes)
                if (n.pitch == pitch && n.startFrame == want) return true;
            return false;
        });
    };
    const Frame drawFrame = 7000;         // the test's own frame, not an x
    Frame clipStart = 0;
    if (win->LockWithTimeout(1000000) == B_OK) {
        clipStart = roll->RegionStart();
        win->Unlock();
    }
    const Frame want16 = SnapClipRelative(project.tempoMap, drawFrame, clipStart,
                                          SnapGrid{ SnapKind::Sixteenth, false });
    drawAt(drawFrame, 93);
    CHECK(waitForNoteAt(93, want16));
    Shot("roll-grid-16th");

    // The same point on the quarter grid lands on a different frame: 1/4 is
    // coarser, so the two cannot agree unless the grid is ignored.
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            roll->SetSnapGrid(SnapGrid{ SnapKind::Quarter, false });
            win->Unlock();
        }
        bool snapOk = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            snapOk = roll->Snap().kind == SnapKind::Quarter && roll->Snap().On();
            win->Unlock();
        }
        CHECK(snapOk);
    }
    const Frame want4 = SnapClipRelative(project.tempoMap, drawFrame, clipStart,
                                         SnapGrid{ SnapKind::Quarter, false });
    CHECK(want4 != want16);          // the two grids really do differ here
    drawAt(drawFrame, 92);           // a new pitch row: a NEW note's frame
    CHECK(waitForNoteAt(92, want4));
    Shot("roll-grid-quarter");
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            roll->SetSnapGrid(SnapGrid{ SnapKind::Sixteenth, false });
            win->Unlock();
        }
    }

    // --- F. A CC point drags; it does not paint a staircase -----------------
    // Two points of the same controller, then a drag that STARTS on the first
    // one: the count must not change (the old code wrote a new point at every
    // new x and left the grabbed one behind).
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            roll->SetLaneCc(1);
            win->Unlock();
        }
        {
            std::vector<MidiClipEvent> ev;
            for (int i = 0; i < 2; i++) {
                MidiClipEvent e;
                e.type = MidiClipEvent::CC;
                e.data = 1;
                e.startFrame = 3000 + i * 6000;   // clip-relative
                e.value = 40 + i * 60;
                ev.push_back(e);
            }
            win->LockWithTimeout(1000000);
            stack->Execute(std::make_unique<SetMidiClipEventsCommand>(
                               tid, cid, std::move(ev)), project);
            win->Unlock();
        }
        win->PostMessage(kMsgReloadEngine);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = roll->EventCount() == 2 && roll->Lane() == 1;
            win->Unlock();
            return ok;
        }));
        Shot("roll-cc-lane");

        // The point at 3000, grabbed at its own x and its own y (the lane's
        // value->y mapping), dragged by a frame delta the test chooses.
        float x0 = 0.0f, x1 = 0.0f, y0 = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            x0 = roll->XForFrameForTest(3000);
            x1 = roll->XForFrameForTest(3000 + 11520);
            y0 = roll->LaneValueYForTest(40);
            win->Unlock();
        }
        const float y1 = y0 + Themed(12.0f);
        std::printf("  cc aim: (%.1f, %.1f) -> (%.1f, %.1f), laneTop %.0f\n",
                    x0, y0, x1, y1, laneTop);
        CHECK(x0 >= Themed(58.0f) && x0 <= viewW);
        CHECK(x1 >= Themed(58.0f) && x1 <= viewW);
        CHECK(y0 > laneTop && y0 <= viewH);          // inside the lane
        click(B_MOUSE_DOWN,  BPoint(x0, y0), 0, 1);
        click(B_MOUSE_MOVED, BPoint(x1, y1), 0, 1);
        click(B_MOUSE_UP,    BPoint(x1, y1), 0, 0);
        snooze(150000);

        // Wait for the dragged point to be at a frame that is NEITHER of the
        // two the model started with: that is the commit landing.
        std::vector<MidiClipEvent> after;
        CHECK(WaitFor([&] {
            MidiClip c;
            if (!modelClip(&c)) return false;
            for (const MidiClipEvent& e : c.events)
                if (e.type == MidiClipEvent::CC && e.data == 1
                    && e.startFrame != 3000 && e.startFrame != 9000) {
                    after = c.events;
                    return true;
                }
            return false;
        }));
        if (after.empty()) {
            MidiClip c;
            modelClip(&c);
            std::printf("  cc drag did not land: %d events\n", (int)c.events.size());
            for (const MidiClipEvent& e : c.events)
                std::printf("    type %d data %d frame %lld value %d\n",
                            (int)e.type, (int)e.data, (long long)e.startFrame,
                            (int)e.value);
        }
        int points = 0;
        Frame movedPre = -1;
        for (const MidiClipEvent& e : after)
            if (e.type == MidiClipEvent::CC && e.data == 1) {
                points++;
                if (e.startFrame != 9000) movedPre = e.startFrame;
            }
        CHECK(points == 2);              // no staircase of new points
        CHECK(movedPre > 3000);          // the grabbed point moved, in time
        Shot("roll-cc-drag");
        if (win->LockWithTimeout(1000000) == B_OK) {
            roll->SetLaneCc(-1);
            win->Unlock();
        }
    }

    // --- B. The region moves under the roll --------------------------------
    // The timeline drags a MIDI region with MoveMidiClipCommand (the id stays,
    // only startFrame moves) and posts kMsgReloadEngine. The roll must re-point
    // (its notes are clip-relative) and its next gesture must not write the old
    // absolute position back into the model.
    const Frame newStart = 48000;
    {
        win->LockWithTimeout(1000000);
        stack->Execute(std::make_unique<MoveMidiClipCommand>(tid, cid, newStart),
                       project);
        win->Unlock();
    }
    win->PostMessage(kMsgReloadEngine);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = roll->RegionStart() == newStart;
        win->Unlock();
        return ok;
    }));
    {
        MidiClip c;
        modelClip(&c);
        const std::vector<MidiNote> shown = rollNotes();
        CHECK(shown.size() == c.notes.size());   // still this region's notes
    }
    Shot("roll-region-moved");

    // --- B2. A split trims the half; the roll must not grow it back ---------
    const Frame halfLen = 24000;
    Frame preSplitLen = 0;
    {
        MidiClip c;
        modelClip(&c);
        preSplitLen = c.lengthFrames;
    }
    CHECK(preSplitLen > halfLen);
    {
        win->LockWithTimeout(1000000);
        stack->Execute(std::make_unique<SplitMidiClipCommand>(tid, cid,
                                                              newStart + halfLen),
                       project);
        win->Unlock();
    }
    win->PostMessage(kMsgReloadEngine);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = roll->RegionLength() == halfLen;
        win->Unlock();
        return ok;
    }));
    // A gesture inside the half: the region must not go back to its PRE-SPLIT
    // length (the bug swallowed the split). It may still grow past the half
    // when a note's tail runs past the new end -- the notes command's
    // documented "only ever grows" rule -- so the check is against the old
    // length, which is what the bug restored.
    {
        const std::vector<MidiNote> beforeDrag = rollNotes();
        int idx = -1;
        widestNote(&idx);
        dragNote(idx, 7000);
        CHECK(WaitFor([&] {
            MidiClip c2;
            if (!modelClip(&c2)) return false;
            return !NotesEqual(c2.notes, beforeDrag);
        }));   // the gesture landed
        MidiClip c;
        modelClip(&c);
        // The region must cover its OWN notes and nothing else. The bug grew it
        // back over the notes the split had taken away (and over the length the
        // region had before the split), so a length that exceeds the end of
        // every note it now holds is the signature to catch -- a length that
        // merely grew to cover a note's tail is the notes command's documented
        // "only ever grows" rule and is allowed.
        Frame ownEnd = 0;
        for (const MidiNote& n : c.notes)
            ownEnd = std::max(ownEnd, n.startFrame
                                       + std::max<Frame>(1, n.lengthFrames));
        std::printf("  after the split gesture: len %lld (half %lld, own notes "
                    "end %lld, was %lld)\n",
                    (long long)c.lengthFrames, (long long)halfLen,
                    (long long)ownEnd, (long long)preSplitLen);
        CHECK(c.lengthFrames <= std::max(ownEnd, halfLen));
        CHECK(c.startFrame == newStart);
    }
    Shot("roll-after-split");

    // --- C. Follow is the View menu's policy, both ways ---------------------
    auto followOf = [&]() -> int {
        int v = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            v = roll->Follow() ? 1 : 0;
            win->Unlock();
        }
        return v;
    };
    auto followMarked = [&]() -> int {
        int v = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (BMenuBar* mb = win->KeyMenuBar())
                for (int32 i = 0; i < mb->CountItems(); i++)
                    if (BMenuItem* it = mb->ItemAt(i)) {
                        const char* l = it->Label();
                        if (l && std::strcmp(l, "View") == 0 && it->Submenu())
                            for (int32 j = 0; j < it->Submenu()->CountItems(); j++)
                                if (BMenuItem* vi = it->Submenu()->ItemAt(j)) {
                                    const char* vl = vi->Label();
                                    if (vl && std::strcmp(vl, "Follow Playhead") == 0)
                                        v = vi->IsMarked() ? 1 : 0;
                                }
                    }
            win->Unlock();
        }
        return v;
    };
    CHECK(followOf() == 1);                  // opens following, like the menu
    win->PostMessage(MSG_FOLLOW_TEST);
    CHECK(WaitFor([&] { return followOf() == 0; }));
    CHECK(followMarked() == 0);              // the mark agrees with the roll
    win->PostMessage(MSG_FOLLOW_TEST);
    CHECK(WaitFor([&] { return followOf() == 1; }));
    CHECK(followMarked() == 1);

    // --- H. The clipboard, from the docked editor ---------------------------
    // The docked roll is a VIEW of the main window, so the menu bar claims
    // Cmd-C first. Haiku skips the shortcut of a DISABLED item but still
    // swallows the key, so there are two halves to check: the Edit menu's Copy
    // is enabled when the EDITOR has a selection (not only when the arrangement
    // has one), and the key then reaches the roll's clipboard and pastes back
    // as a model edit. The menu is refreshed through its own entry point --
    // the test cannot wait on the 2 Hz poll, and reading the item before the
    // poll ran is exactly how this check failed the first time.
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            roll->SelectAll();
            win->UpdateEditMenu();   // what the poll would do, now
            win->Unlock();
        }
        bool copyEnabled = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (BMenuBar* mb = win->KeyMenuBar())
                for (int32 i = 0; i < mb->CountItems(); i++)
                    if (BMenuItem* it = mb->ItemAt(i)) {
                        const char* l = it->Label();
                        if (l && std::strcmp(l, "Edit") == 0 && it->Submenu())
                            for (int32 j = 0; j < it->Submenu()->CountItems(); j++)
                                if (BMenuItem* ei = it->Submenu()->ItemAt(j)) {
                                    const char* el = ei->Label();
                                    if (el && std::strcmp(el, "Copy") == 0)
                                        copyEnabled = ei->IsEnabled();
                                }
                    }
            win->Unlock();
        }
        CHECK(copyEnabled);
        // The key itself, through the focus view's token (see postKey): the
        // menu bar's shortcut is what delivers it, which is why the item's
        // enabled state above is the gate the user meets too.
        postKey("c", B_COMMAND_KEY);
        bool haveClip = false;
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            haveClip = !roll->ClipboardEmpty();
            win->Unlock();
            return haveClip;
        }));
        // Paste it: the model gains notes (the undo below is the proof the
        // commit went through the stack, not around it).
        const int notesBefore = (int)rollNotes().size();
        postKey("v", B_COMMAND_KEY);
        CHECK(WaitFor([&] {
            MidiClip c;
            if (!modelClip(&c)) return false;
            return (int)c.notes.size() > notesBefore;
        }));
        Shot("roll-clipboard");
        win->PostMessage(MSG_UNDO_TEST);   // leave the region as it was
        CHECK(WaitFor([&] {
            MidiClip c;
            if (!modelClip(&c)) return false;
            return (int)c.notes.size() == notesBefore;
        }));
    }

    // --- H2. Alt: a click toggles the selection, a DRAG duplicates ----------
    // Both gestures share the Alt bits (on a PC keyboard Alt IS B_COMMAND_KEY,
    // which used to mean only "toggle"), and which one it is depends on whether
    // the pointer moves.
    {
        auto postMouse = [&](uint32 what, BPoint p, uint32 mods) {
            click(what, p, mods, what == B_MOUSE_UP ? 0 : 1);
        };
        // Aimed at the widest note's own row and its MIDDLE, like every other
        // click here (an edge would be a resize; a one-frame note cannot be
        // grabbed at all).
        int altIdx = -1;
        const MidiNote altNote = widestNote(&altIdx);
        float nx = 0.0f, ny = 0.0f;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const Frame mid = altNote.startFrame
                            + std::max<Frame>(1, altNote.lengthFrames) / 2;
            nx = roll->XForFrameForTest(mid);
            ny = rowY(altNote.pitch);
            roll->SelectAll();
            win->Unlock();
        }
        CHECK(aimOk(nx, ny));
        int selBefore = -1, notesBefore = 0;
        MidiClip c0;
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            selBefore = roll->SelectedCountForTest();
            win->Unlock();
            modelClip(&c0);
            notesBefore = (int)c0.notes.size();
            return selBefore > 0;
        }));
        // The Alt-click toggle: selected -> not selected, nothing committed.
        postMouse(B_MOUSE_DOWN, BPoint(nx, ny), B_COMMAND_KEY);
        snooze(80000);
        postMouse(B_MOUSE_UP, BPoint(nx, ny), B_COMMAND_KEY);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const int s2 = roll->SelectedCountForTest();
            win->Unlock();
            return s2 == selBefore - 1;
        }));
        {
            MidiClip c1;
            modelClip(&c1);
            CHECK((int)c1.notes.size() == notesBefore);   // a toggle is not an edit
        }
        Shot("roll-alt-click");

        // The Alt-drag duplicate: the model gains copies (and one undo takes
        // them back, so it went through the stack).
        if (win->LockWithTimeout(1000000) == B_OK) { roll->SelectAll(); win->Unlock(); }
        postMouse(B_MOUSE_DOWN, BPoint(nx, ny), B_COMMAND_KEY);
        snooze(80000);
        postMouse(B_MOUSE_MOVED, BPoint(nx + 40.0f, ny), B_COMMAND_KEY);
        snooze(80000);
        postMouse(B_MOUSE_UP, BPoint(nx + 40.0f, ny), B_COMMAND_KEY);
        CHECK(WaitFor([&] {
            MidiClip c2;
            if (!modelClip(&c2)) return false;
            return (int)c2.notes.size() > notesBefore;
        }));
        Shot("roll-alt-drag");
        win->PostMessage(MSG_UNDO_TEST);
        CHECK(WaitFor([&] {
            MidiClip c3;
            if (!modelClip(&c3)) return false;
            return (int)c3.notes.size() == notesBefore;
        }));
        if (win->LockWithTimeout(1000000) == B_OK) { roll->SelectAll(); win->Unlock(); }
    }

    // --- D. Audition: a click on the keyboard column sounds -----------------
    // The engine side cannot be heard by a test, so the engine counts the
    // audition notes its callback actually RENDERS (Engine::
    // AuditionNotesRendered). That is the end-to-end proof of the path the
    // click takes: the view's post, MainWindow's engine, the graph's bus for
    // the region's track, and the instrument render. It is also the regression
    // check for the bug this test would have caught: Bus's hand-written move
    // constructor dropped the reserved audition list, so the graph's bus had
    // capacity 0 and every click was silently dropped.
    {
        const uint64_t before = win->AuditionNotesRendered();
        const float kx = Themed(20.0f);            // inside the keyboard column
        const float ky = rowY(96);                 // a row the pane can show
        click(B_MOUSE_DOWN, BPoint(kx, ky), 0, 1);
        snooze(120000);
        click(B_MOUSE_UP,   BPoint(kx, ky), 0, 0);
        CHECK(WaitFor([&] {
            return win->AuditionNotesRendered() > before;
        }));
        std::printf("  audition: %llu notes rendered\n",
                    (unsigned long long)win->AuditionNotesRendered());
        Shot("roll-audition");
        // Put the idle engine away (the test's own click raised it).
        win->PostMessage(MSG_AUDITION_IDLE_TEST);
        CHECK(WaitFor([&] { return !win->AuditionEngineUp(); }));
    }

    // --- The region is gone: the editor closes -----------------------------
    {
        win->LockWithTimeout(1000000);
        stack->Execute(std::make_unique<RemoveMidiClipCommand>(tid, cid), project);
        win->Unlock();
    }
    win->PostMessage(kMsgReloadEngine);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool gone = win->FindView("roll") == nullptr;
        win->Unlock();
        return gone;
    }));
    Shot("roll-region-gone");

    // Leave the dock as the test found it (the region is already gone).
    win->PostMessage(MSG_TOGGLE_DOCK);
    snooze(150000);
}

// M7.1's sidechain picker (package 05). The picker is a BPopUpMenu, which is
// modal and cannot be driven by a posted message, so what is driven here is
// what its selection POSTS: the kMsgApplyFx snapshot, which must carry the
// source through EncodeFxChain into the model (and therefore into the file and
// the engine). The shot is what proves the row is on screen and readable.
static void TestSidechainPicker(MainWindow* win, Project& project) {
    std::printf("sidechain picker ...\n");
    // The panel's codec carries BOTH descriptor fields M7.1 and M7.4 added: the
    // key source and the plugin's own state. They were added on two branches,
    // and a BMessage refuses a second type under one field name -- so a shared
    // name would silently drop whichever was added second.
    {
        EffectDesc d = CompressorDesc();
        d.sidechainSource = 42;
        d.state = "a plugin's own state";
        BMessage m(kMsgApplyFx);
        EncodeFxChain(m, { d, CompressorDesc() });
        const std::vector<EffectDesc> back = DecodeFxChain(m);
        CHECK(back.size() == 2);
        if (back.size() == 2) {
            CHECK(back[0].sidechainSource == 42);
            CHECK(back[0].state == "a plugin's own state");
            CHECK(back[1].sidechainSource == kInvalidTrackId);
            CHECK(back[1].state.empty());
        }
    }
    Track kick = MakeMidiTrack(project, {}, "sc-kick");
    Track bass = MakeMidiTrack(project, {}, "sc-bass");
    const TrackId kid = kick.id, bid = bass.id;
    bass.fx.push_back(CompressorDesc());
    CHECK(LockedAddTrack(win, project, kick));
    CHECK(LockedAddTrack(win, project, bass));

    const int before = VisibleWindows();
    BMessage open(kMsgMixFx);
    open.AddInt64("track", (int64)bid);
    open.AddInt32("slot", 0);              // the editor's single-insert view
    win->PostMessage(&open);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
    Shot("fx-panel-sidechain");

    // Exactly what the picker's mouse-up posts: the whole chain, with the
    // source set and the appended extKey slot switched on.
    std::vector<EffectDesc> chain;
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (const Track* t = project.FindTrack(bid)) chain = t->fx;
        win->Unlock();
    }
    CHECK(!chain.empty());
    if (!chain.empty()) {
        chain[0].sidechainSource = kid;
        chain[0].params.resize((size_t)kExtKeySlot + 1, 0.0f);
        chain[0].params[(size_t)kExtKeySlot] = 1.0f;
        BMessage edit(kMsgApplyFx);
        edit.AddInt64("track", (int64)bid);
        EncodeFxChain(edit, chain);
        win->PostMessage(&edit);
    }
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const Track* t = project.FindTrack(bid);
        const bool ok = t && !t->fx.empty()
                     && t->fx[0].sidechainSource == kid
                     && t->fx[0].p((size_t)kExtKeySlot) == 1.0f;
        win->Unlock();
        return ok;
    }));

    // ...and picking "None" again posts the same shape with the key off.
    if (win->LockWithTimeout(1000000) == B_OK) {
        if (const Track* t = project.FindTrack(bid)) chain = t->fx;
        win->Unlock();
    }
    if (!chain.empty()) {
        chain[0].sidechainSource = kInvalidTrackId;
        chain[0].params[(size_t)kExtKeySlot] = 0.0f;
        BMessage edit(kMsgApplyFx);
        edit.AddInt64("track", (int64)bid);
        EncodeFxChain(edit, chain);
        win->PostMessage(&edit);
    }
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const Track* t = project.FindTrack(bid);
        const bool ok = t && !t->fx.empty()
                     && t->fx[0].sidechainSource == kInvalidTrackId
                     && t->fx[0].p((size_t)kExtKeySlot) == 0.0f;
        win->Unlock();
        return ok;
    }));

    // Close the editor, so the window count is where the next test expects it.
    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w == nullptr || w == win || w->IsHidden()) continue;
        if (w->LockWithTimeout(1000000) == B_OK) w->Quit();
        break;
    }
    CHECK(WaitQuiet());
    HideOtherWindows(win);
}

// M4.1: a rebuild during playback builds the graph on the engine's worker
// thread and swaps it in — the transport must not stop, the device must not be
// re-opened (no BSoundPlayer re-create) and the playhead must keep advancing
// across the swap. The plan asks for exactly this check ("an engine swap during
// playback with no BSoundPlayer re-create (an engine counter is exposed for
// tests)").
static void TestEngineGraphSwap(MainWindow* win, Project& project,
                                CommandStack& stack) {
    std::printf("engine graph swap ...\n");

    // A real (short) WAV for the clips, so the engine really opens a stream.
    const char* wav = "/tmp/haiku_daw_ui_swap.wav";
    {
        WavWriter w;
        if (w.Open(wav, 48000, 2)) {
            std::vector<int16_t> frames(48000 * 2 * 5, 0);   // 5 s of silence
            CHECK(w.WriteInt16(frames.data(), frames.size()));
            CHECK(w.Close());
        }
    }

    // Two tracks with a clip each — enough to have a graph worth rebuilding,
    // small enough that the rebuild is quick.
    TrackId first = kInvalidTrackId;
    for (int t = 0; t < 2; t++) {
        Track track;
        track.id = project.NextTrackId();
        track.type = TrackType::Audio;
        track.name = "swap-" + std::to_string(t);
        track.gain = 1.0f;
        Clip clip;
        clip.id = project.NextClipId();
        clip.sourcePath = wav;
        clip.startFrame = 0;
        clip.lengthFrames = 48000 * 5;
        track.clips.push_back(clip);
        CHECK(LockedAddTrack(win, project, track));
        if (t == 0) first = track.id;
    }

    // Play from the top with no loop: the rebuild under test must not depend on
    // what an earlier flow left the transport in.
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead = 0;
        project.transport.loopEnabled = false;
        win->Unlock();
    }
    {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return win->IsPlaying(); }, 5000000));
    // The first graph for this project may still be building: the counters below
    // describe the state before the rebuild, and the swap counter must move on
    // its own.
    CHECK(WaitFor([&] { return win->EngineGraphReady(); }, 20000000));

    const uint64_t swaps0   = win->EngineGraphsPublished();
    const uint64_t players0 = win->EnginePlayersOpened();
    const uint64_t starts0  = win->EnginePlayerStarts();
    const Frame    ph0      = win->EnginePlayhead();

    // A structural edit while playing: an insert added to the first track,
    // through the command stack, then the message a chain edit posts. SyncFx
    // reports the structural mismatch, so this is the "rebuild the engine now"
    // path (ReloadActiveEngine) under test.
    {
        if (win->LockWithTimeout(1000000) == B_OK) {
            std::vector<EffectDesc> chain;
            if (const Track* t = project.FindTrack(first)) chain = t->fx;
            EffectDesc d;
            d.type = EffectType::Delay;   // a built-in, always available
            chain.push_back(d);
            stack.Execute(std::make_unique<SetFxCommand>(first, false,
                                                         std::move(chain)),
                          project);
            win->Unlock();
        }
        win->PostMessage(kMsgFxChanged);
        win->PostMessage(kMsgUiRefresh);
    }

    CHECK(WaitFor([&] { return win->EngineGraphsPublished() > swaps0; },
                  20000000));
    CHECK(win->EnginePlayersOpened() == players0);   // device NOT re-opened
    CHECK(win->EnginePlayerStarts() == starts0);     // transport NOT restarted
    CHECK(win->IsPlaying());
    // The old graph kept rolling while the new one was built, so the playhead
    // moved straight through the swap (this is the "no gap" half of M4.1).
    CHECK(win->EnginePlayhead() > ph0);
    Shot("engine-graph-swap");

    {
        BMessage key(B_KEY_DOWN);   // space again: stop
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));
}

// M1.5's measurement: a project the size the plan names (32 tracks, ~300
// clips) must start playing quickly, and the timeline must draw it in under
// 4 ms a frame. The draw time is reported by TimelineView itself under
// DAW_TIMELINE_TIMING (see the DrawTimer); this check builds the project,
// starts the transport, and asserts what the test can see -- that playback
// started, and started promptly.
// M4.2: the loop is walked by the RUNNING graph. Every pass used to go through
// the window: the pulse saw the playhead reach the loop end, rewound it and
// called StartPlayback -- a position-changing rebuild, which detaches the graph
// (silence, the playhead frozen) and builds a whole new one (every clip file
// re-opened, every disk thread restarted, every ring primed). On the 32-track
// project that is seconds of hole per pass.
//
// This runs a real loop on the real engine and proves the wrap is the engine's
// own: across several passes the wrap counter moves while the published-graph
// count, the player count and the player-start count do not move at all --
// i.e. nothing was re-opened, nothing was re-built, and the device never
// stopped. The playhead it reports stays inside the loop, which is what the
// timeline draws.
static void TestSeamlessLoop(MainWindow* win, Project& project) {
    std::printf("seamless loop ...\n");

    // One real WAV, long enough to cover the loop and the material before it.
    // What the samples ARE does not matter: nothing is audible on the VM, so
    // what this test can prove is the transport and the counters.
    const char* wav = "/tmp/haiku_daw_ui_loop.wav";
    {
        WavWriter w;
        if (w.Open(wav, 48000, 2)) {
            std::vector<int16_t> frames(48000 * 2 * 6, 0);   // 6 s of silence
            CHECK(w.WriteInt16(frames.data(), frames.size()));
            CHECK(w.Close());
        }
    }

    TrackId trackId = kInvalidTrackId;
    {
        Track track;
        track.id = project.NextTrackId();
        track.type = TrackType::Audio;
        track.name = "loop";
        track.gain = 1.0f;
        Clip clip;
        clip.id = project.NextClipId();
        clip.sourcePath = wav;
        clip.startFrame = 0;
        clip.lengthFrames = 48000 * 6;
        track.clips.push_back(clip);
        CHECK(LockedAddTrack(win, project, track));
        trackId = track.id;
    }

    // A 1 s loop starting 2 s in, played from the top: the transport plays
    // [0, loopEnd), wraps, and then stays on the loop.
    const Frame loopStart = 96000, loopEnd = 144000;
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.playhead   = 0;
        project.transport.loopStart  = loopStart;
        project.transport.loopEnd    = loopEnd;
        project.transport.loopEnabled = true;
        win->Unlock();
    }
    {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return win->IsPlaying(); }, 5000000));
    CHECK(WaitFor([&] { return win->EngineGraphReady(); }, 20000000));

    const uint64_t swaps0   = win->EngineGraphsPublished();
    const uint64_t players0 = win->EnginePlayersOpened();
    const uint64_t starts0  = win->EnginePlayerStarts();
    const uint64_t wraps0   = win->EngineWraps();

    // Three wraps: enough that the old code would have rebuilt the engine three
    // times (and, before M4.1, three times on the window thread).
    const bigtime_t t0 = system_time();
    CHECK(WaitFor([&] { return win->EngineWraps() >= wraps0 + 3; }, 20000000));
    const bigtime_t ms = system_time() - t0;
    const uint64_t wraps = win->EngineWraps() - wraps0;
    std::printf("  loop: %llu wraps in %.0f ms (%.0f ms per pass)\n",
                (unsigned long long)wraps, (double)ms / 1000.0,
                (double)ms / 1000.0 / (double)(wraps > 0 ? wraps : 1));

    CHECK(wraps >= 3);
    // Nothing was rebuilt, nothing was re-opened, the device never stopped:
    // this is the whole point of M4.2.
    CHECK(win->EngineGraphsPublished() == swaps0);
    CHECK(win->EnginePlayersOpened() == players0);
    CHECK(win->EnginePlayerStarts() == starts0);
    CHECK(win->IsPlaying());                    // and it is still rolling
    // The transport is inside the loop, wherever in a pass it happens to be.
    // The bound is inclusive at the loop end: a callback whose block ends
    // exactly there leaves the playhead ON it until the next block wraps.
    {
        const Frame ph = win->EnginePlayhead();
        std::printf("  loop: playhead %lld, loop [%lld, %lld)\n",
                    (long long)ph, (long long)loopStart, (long long)loopEnd);
        CHECK(ph >= loopStart && ph <= loopEnd);
    }
    // ~1 s of audio per pass, at whatever the VM manages: a bound so a wrap
    // that took the old path (a whole graph build) fails here.
    CHECK(ms < 20000000);

    snooze(500000);            // let the timeline draw the loop while it rolls
    Shot("seamless-loop");

    {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));
    // Leave the transport as the next test expects to find it.
    if (win->LockWithTimeout(1000000) == B_OK) {
        project.transport.loopEnabled = false;
        project.transport.playhead = 0;
        win->Unlock();
    }
}

// M4.4: waveform envelopes are built OFF the window thread and cached on disk.
//
// `RebuildPeaks` used to run inline: for every clip, open the file and read it
// top to bottom while the window was locked, on every load, keeping nothing.
// This test proves the two things that changed — the window stays responsive
// while the envelopes are being built, and a second pass over the same media
// reads them back from the cache instead of decoding the audio again.
static void TestPeakCache(MainWindow* win, Project& project) {
    std::printf("peak cache ...\n");

    // A file big enough that scanning it is not instantaneous: 20 s of a signal.
    const char* wav = "/tmp/haiku_daw_ui_peaks.wav";
    {
        WavWriter w;
        if (w.Open(wav, 48000, 2)) {
            std::vector<int16_t> f(48000 * 2 * 20);
            for (size_t i = 0; i < f.size(); i++)
                f[i] = (int16_t)((i / 32) % 200 - 100);
            CHECK(w.WriteInt16(f.data(), f.size()));
            CHECK(w.Close());
        }
    }
    {
        Track track;
        track.id = project.NextTrackId();
        track.type = TrackType::Audio;
        track.name = "peaks";
        track.gain = 1.0f;
        Clip clip;
        clip.id = project.NextClipId();
        clip.sourcePath = wav;
        clip.startFrame = 0;
        clip.lengthFrames = 48000 * 20;
        track.clips.push_back(clip);
        CHECK(LockedAddTrack(win, project, track));
    }

    win->RebuildPeaksForTest();
    // The window is FREE while that runs. The old code called the scan from its
    // own message handler -- inside the window lock -- so this lock would time
    // out; now the scan is a worker's and the window only drains the results.
    if (win->PeaksLoading()) {
        CHECK(win->LockWithTimeout(1000000) == B_OK);
        win->Unlock();
    }
    // Every clip's file ends up with an envelope — and wait for the PASS to
    // finish, not merely for the first one: a new pass supersedes a running one
    // (that is what keeps a reload from drawing the old project), and it abandons
    // the files it had not reached yet, cache writes included.
    CHECK(WaitFor([&] { return !win->PeaksLoading() && win->PeaksLoaded() > 0; },
                  30000000));
    const int loaded = win->PeaksLoaded();
    std::printf("  peaks: %d envelopes, %d from cache, %d built\n",
                loaded, win->PeakCacheHits(), win->PeakCacheMisses());
    CHECK(loaded >= 1);

    // A second pass over the SAME media must come from the cache: that is the
    // only way the VM can show the on-disk cache is doing anything, and it is
    // what makes reopening a project cheap.
    win->RebuildPeaksForTest();
    CHECK(WaitFor([&] { return !win->PeaksLoading()
                               && win->PeakOutcome(wav) != 0; }, 20000000));
    const int hits = win->PeakCacheHits();
    const int misses = win->PeakCacheMisses();
    // The claim is about THIS test's file — the project's other clips point at
    // media that may not be on this machine at all, and a file that is missing is
    // skipped rather than cached. So: this file came back from the cache, and
    // some of the pass did too.
    std::printf("  peaks: second pass %d hits, %d misses, this file -> %d "
                "(1 = cache)\n", hits, misses, win->PeakOutcome(wav));
    CHECK(win->PeakOutcome(wav) == 1);
    CHECK(hits >= 1);
    CHECK(win->PeaksLoaded() >= loaded);
}

static void TestBigProjectPlayback(MainWindow* win, Project& project) {
    std::printf("big project playback ...\n");

    // One real (if short) WAV for every clip: the point is the count. It carries
    // a SIGNAL (a quiet square, so every frame is non-zero), because the test
    // asks whether sound came out at all (M4.3) — silence cannot tell "the graph
    // landed" from "the streams are open and delivering".
    const char* wav = "/tmp/haiku_daw_ui_big.wav";
    {
        WavWriter w;
        if (w.Open(wav, 48000, 2)) {
            std::vector<int16_t> frames(48000 * 2);
            for (size_t i = 0; i < frames.size(); i++)
                frames[i] = ((i / 64) % 2) ? 8000 : -8000;
            CHECK(w.WriteInt16(frames.data(), frames.size()));
            CHECK(w.Close());
        }
    }

    size_t tracksBefore = 0;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tracksBefore = project.Tracks().size();
        win->Unlock();
    }
    for (int t = 0; t < 32; t++) {
        Track track;
        track.id = project.NextTrackId();
        track.type = TrackType::Audio;
        track.name = "big-" + std::to_string(t);
        track.gain = 1.0f;
        for (int c = 0; c < 10; c++) {          // 320 clips
            Clip clip;
            clip.id = project.NextClipId();
            clip.sourcePath = wav;
            clip.startFrame = (Frame)c * 96000;   // 2 s apart
            clip.lengthFrames = 48000;
            clip.sourceOffset = 0;
            track.clips.push_back(clip);
        }
        CHECK(LockedAddTrack(win, project, track));
    }
    if (win->LockWithTimeout(1000000) == B_OK) {
        CHECK(project.Tracks().size() == tracksBefore + 32);
        project.transport.playhead = 0;
        win->Unlock();
    }

    // Play: the window must roll promptly (the plan's 300 ms budget). Space
    // is the transport toggle -- the same route the keyboard test uses.
    //
    // M4.1: the graph (one stream per clip, 320 of them) is no longer built on
    // the window thread. The window only queues the rebuild and starts the
    // player, so what is timed here is the WINDOW's response; the graph then
    // lands on the worker thread, and that second number is the one the 300 ms
    // audio budget still measures against (the disk-stream pool is M4.3's).
    auto playing = [&] { return win->IsPlaying(); };
    const uint64_t swaps0 = win->EngineGraphsPublished();
    const bigtime_t t0 = system_time();
    {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return playing(); }, 5000000));
    const bigtime_t uiMs = system_time() - t0;
    CHECK(WaitFor([&] { return win->EngineGraphsPublished() > swaps0; },
                  60000000));
    const bigtime_t readyMs = system_time() - t0;
    std::printf("  big project: %zu tracks, %zu clips, window rolls in %.0f ms, "
                "graph swapped in %.0f ms\n",
                tracksBefore + 32, (size_t)(32 * 10),
                (double)uiMs / 1000.0, (double)readyMs / 1000.0);
    // MEASURED 2026-10-09, before M4.1 (VM, beta6, 2 vCPU): 3.1 s — the window
    // thread built all 320 streams itself. M4.1 moves that to the worker, so the
    // WINDOW budget below is the one it fixes: the transport rolls without
    // waiting for a single file open.
    CHECK(uiMs < 300000);
    // The graph still lands. M4.1 measured 2592 ms and M4.2 3439 ms for this
    // project (39 tracks then); M4.3's stream pool and the batched priming bring
    // the ENGINE's own build inside the plan's 300 ms — that is the number below,
    // measured on the builder thread, and it is the plan's audio-start budget.
    // The window's message latency (`uiMs` above) is a separate thing and is not
    // the engine's to fix; the end-to-end `readyMs` is printed for the record.
    const bigtime_t buildUs = win->EngineBuildUs();
    std::printf("  big project: engine build %llu us, "
                "command-to-graph %.0f ms\n",
                (unsigned long long)buildUs, (double)readyMs / 1000.0);
    // MEASURED 2026-10-12 on the VM (2 vCPU): 329 streams, 135 ms. This is the
    // plan's 300 ms audio-start budget, measured where the engine owns it. The
    // bound is deliberately tight (3x the measurement): 3295 ms is what a
    // per-clip prime wait costs, and 462 ms is what the pool costs if a stream
    // whose source has ended is allowed to make the build wait — both are
    // regressions this must fail on.
    CHECK(buildUs > 0 && buildUs < 400000);
    CHECK(readyMs < 12000000);
    // Sound actually comes out: the pool is filling the clips at the playhead,
    // so the master meter reads a level. This is the end-to-end guard for the
    // stream pool — counters alone would pass with every stream silent.
    CHECK(WaitFor([&] { return win->MasterPeakL() > 0.0f; }, 5000000));
    std::printf("  big project: master peak %.3f, sound %.0f ms after the play "
                "command (%.0f ms after the graph landed)\n",
                win->MasterPeakL(), (double)(system_time() - t0) / 1000.0,
                (double)(system_time() - t0 - readyMs) / 1000.0);
    snooze(2000000);            // let the timeline draw it for a while
    Shot("big-project-playing");
    {
        BMessage key(B_KEY_DOWN);   // space again: stop
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
    }
    CHECK(WaitFor([&] { return !playing(); }));
}

// --- M3.2: one keyboard map, one help window -------------------------------

// The shortcuts window is opened by its menu item and shows the text the ONE
// key table generates -- read back out of the real BTextView, so a window that
// shipped a stale hand-written list would fail here. Asking twice must bring
// the open window forward instead of stacking a second one.
static void TestShortcutsWindow(MainWindow* win) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("keyboard shortcuts window ...\n");

    auto findKeys = [&]() -> BWindow* {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strcmp(WindowTitle(w), "Keyboard Shortcuts") == 0)
                return w;
        }
        return nullptr;
    };
    auto countKeys = [&] {
        int n = 0;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strcmp(WindowTitle(w), "Keyboard Shortcuts") == 0)
                n++;
        }
        return n;
    };

    // What the menu item posts (the id is file-local to MainWindow.cpp).
    win->PostMessage(MSG_SHORTCUTS);
    CHECK(WaitFor([&] { return findKeys() != nullptr; }));
    Shot("shortcuts");

    // The text out of the real view, against the same table the behaviour
    // resolves keys through.
    std::string shown;
    {
        BWindow* keys = findKeys();
        BTextView* tv = nullptr;
        if (keys && keys->LockWithTimeout(1000000) == B_OK) {
            tv = dynamic_cast<BTextView*>(keys->FindView(kShortcutsTextView));
            if (tv) {
                shown = tv->Text();
                // The text is drawn in the THEME's text colour on its well
                // colour. A BTextView's runs carry their own colour (not the
                // view's high colour), and the system default was dark-on-dark
                // in Dark mode: the window opened unreadable.
                BFont runFont;
                rgb_color runCol = {};
                tv->GetFontAndColor(0, &runFont, &runCol);
                const rgb_color want = ColText();
                CHECK(runCol.red == want.red && runCol.green == want.green
                      && runCol.blue == want.blue);
                const rgb_color well = ColLcd();
                const rgb_color bg = tv->ViewColor();
                CHECK(bg.red == well.red && bg.green == well.green
                      && bg.blue == well.blue);
            }
            keys->Unlock();
        }
        CHECK(tv != nullptr);
    }
    const std::string expected = BuildShortcutsText();
    CHECK(!shown.empty());
    CHECK(shown == expected);
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        CHECK(shown.find(kKeyBindings[i].keyName) != std::string::npos);
        CHECK(shown.find(kKeyBindings[i].help) != std::string::npos);
    }
    // ... and the plan's five keys are actually in it.
    CHECK(shown.find("Home") != std::string::npos);
    CHECK(shown.find("Record") != std::string::npos);
    CHECK(shown.find("Loop on / off") != std::string::npos);
    CHECK(shown.find("Click (metronome) on / off") != std::string::npos);
    CHECK(shown.find("Enter") != std::string::npos);

    // One window: asking again activates it instead of stacking another.
    win->PostMessage(MSG_SHORTCUTS);
    snooze(400000);
    CHECK(countKeys() == 1);

    HideOtherWindows(win);   // the window is ours; put it away like its close box
    CHECK(WaitQuiet());
}

// The map's transport and navigation keys, driven as real key events: the
// window's own dump of a B_KEY_DOWN is what a keyboard sends.
static void TestKeyMap(MainWindow* win, Project& project, CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("keyboard map ...\n");

    auto sendKey = [&](const char* bytes, uint32 mods = 0) {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", bytes);
        key.AddInt32("modifiers", (int32)mods);
        win->PostMessage(&key);
    };
    auto loopOn = [&] {
        bool on = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            on = project.transport.loopEnabled;
            win->Unlock();
        }
        return on;
    };
    auto playhead = [&] {
        daw::Frame ph = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            ph = project.transport.playhead;
            win->Unlock();
        }
        return ph;
    };
    auto setPlayhead = [&](daw::Frame f) {
        if (win->LockWithTimeout(1000000) == B_OK) {
            project.transport.playhead = f;
            win->Unlock();
        }
    };

    // A focused text field keeps its own keys (the DispatchMessage rule): the
    // tempo field must not toggle the loop.
    BView* tempo = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        tempo = win->FindView("tempo");
        if (tempo) tempo->MakeFocus(true);
        win->Unlock();
    }
    const bool loopBefore = loopOn();
    sendKey("l");
    snooze(300000);
    CHECK(loopOn() == loopBefore);

    // ... and the timeline takes them back on a click.
    BView* timeline = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        timeline = win->FindView("timeline");
        win->Unlock();
    }
    CHECK(timeline != nullptr);
    BMessage down(B_MOUSE_DOWN);
    down.AddInt32("buttons", 1);
    down.AddPoint("where", BPoint(220, 14));   // the ruler
    if (timeline) BMessenger(timeline).SendMessage(&down);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = dynamic_cast<TimelineView*>(win->CurrentFocus()) != nullptr;
        win->Unlock();
        return ok;
    }));

    // L: loop on, then off. (It needs no loop range: the marker-span rule may
    // set one, the bar rule always can, and the flag is what the key owns.)
    sendKey("l");
    CHECK(WaitFor([&] { return loopOn() != loopBefore; }));
    sendKey("l");
    CHECK(WaitFor([&] { return loopOn() == loopBefore; }));

    // L with nothing at all to adopt leaves a range the engine can walk — the
    // BAR around the playhead (package 50, item D). Before this the flag lit
    // while the engine ignored a zero-length loop: the lamp and the status
    // line claimed looping and nothing looped. Markers would win the adoption
    // order (they are tried first), so this case starts with none.
    {
        std::vector<Marker> markersWere;
        if (win->LockWithTimeout(1000000) == B_OK) {
            markersWere = project.markers;      // the marker rule is tried first
            project.markers.clear();            // ...so this is the "nothing" case
            project.transport.loopEnabled = false;
            project.transport.loopStart  = 0;
            project.transport.loopEnd    = 0;
            win->Unlock();
        }
        setPlayhead(120000);                    // inside bar 2 (96000..192000)
        sendKey("l");
        daw::Frame lo = -1, hi = -1;
        CHECK(WaitFor([&] {
            bool on = false;
            if (win->LockWithTimeout(1000000) == B_OK) {
                on = project.transport.loopEnabled;
                lo = project.transport.loopStart;
                hi = project.transport.loopEnd;
                win->Unlock();
            }
            return on && hi > lo;
        }));
        CHECK(lo == 96000);                     // the bar containing the playhead
        CHECK(hi == 192000);
        sendKey("l");                           // and off again
        CHECK(WaitFor([&] { return !loopOn(); }));
        if (win->LockWithTimeout(1000000) == B_OK) {
            project.markers = markersWere;      // put the session back
            project.transport.loopStart = 0;
            project.transport.loopEnd   = 0;
            win->Unlock();
        }
        win->PostMessage(kMsgUiRefresh);
    }

    // C: the click. The window's own state is the observable.
    const bool clickBefore = win->MetronomeOn();
    sendKey("c");
    CHECK(WaitFor([&] { return win->MetronomeOn() != clickBefore; }));
    sendKey("c");
    CHECK(WaitFor([&] { return win->MetronomeOn() == clickBefore; }));

    // Home and Enter: return to zero from wherever the playhead was.
    setPlayhead(96000);
    sendKey("\x01");   // B_HOME
    CHECK(WaitFor([&] { return playhead() == 0; }));
    setPlayhead(48000);
    sendKey("\x0a");   // B_RETURN / B_ENTER
    CHECK(WaitFor([&] { return playhead() == 0; }));

    // R: record, on an armed MIDI track (a MIDI-only take opens no input
    // device and writes no file, so this needs nothing but the transport).
    {
        Track t;
        t.id = project.NextTrackId();
        t.type = TrackType::Midi;
        t.name = "keymap-rec";
        t.armed = true;
        t.input.kind = InputSource::kMidi;   // no audio device involved
        CHECK(LockedAddTrack(win, project, t));
    }
    CHECK(!win->IsRecording());
    sendKey("r");
    CHECK(WaitFor([&] { return win->IsRecording(); }, 10000000));
    sendKey("r");
    CHECK(WaitFor([&] { return !win->IsRecording(); }, 10000000));

    // An unbound key is still the view's (BView::KeyDown), not an action:
    // 'z' toggles nothing.
    const bool clickNow = win->MetronomeOn();
    sendKey("z");
    snooze(200000);
    CHECK(win->MetronomeOn() == clickNow);

    // And the map is what the help shows: the table built the window's text.
    CHECK(std::strcmp(FindKeyBinding('l', 0)->keyName, "L") == 0);
    (void)stack;
}

// --- M3.3: the master meter ------------------------------------------------

// A WAV of `seconds` whose two channels carry the same signal. `amp` is the
// sample amplitude in [0, 1]; `hz` 0 writes a square that reaches full scale
// (the clip fixture).
static std::string WriteMeterWav(const char* path, double amp, double hz,
                                 double seconds = 1.0) {
    std::remove(path);
    const int frames = (int)(48000 * seconds);
    std::vector<int16_t> pcm((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        double s;
        if (hz > 0.0)
            s = amp * std::sin(2.0 * M_PI * hz * i / 48000.0);
        else
            s = amp * ((i / 240) % 2 ? 1.0 : -1.0);   // 100 Hz square
        const int16_t v = (int16_t)std::lround(s * 32767.0);
        pcm[(size_t)i * 2 + 0] = v;
        pcm[(size_t)i * 2 + 1] = v;
    }
    WavWriter w;
    if (!w.Open(path, 48000, 2)) return std::string();
    if (!w.WriteInt16(pcm.data(), pcm.size())) return std::string();
    if (!w.Close()) return std::string();
    return path;
}

// One track playing one file through the engine, then the meter's readings.
// `gain` is the track's fader (the engine's pan law costs 3 dB at centre, so a
// +3.5 dB fader is what puts a full-scale source over 0 dBFS at the meter).
//
// The fixture track is SOLOED and every other track's solo/mute is cleared:
// this is one project for the whole run and earlier tests leave clips on it,
// so anything else still audible would land in the master meter's sum (the
// first cut of this test read a peak that was partly someone else's audio).
// What PlayMeterFixture changed, so the caller can put it back: id + the
// muted/soloed pair each track had BEFORE the fixture silenced the session.
static std::vector<std::tuple<uint64_t, bool, bool>> gMeterSaved;

static void RestoreMeterFixture(MainWindow* win, Project& project) {
    if (win->LockWithTimeout(2000000) != B_OK) return;
    for (const auto& e : gMeterSaved) {
        if (Track* t = project.FindTrack((TrackId)std::get<0>(e))) {
            t->muted  = std::get<1>(e);
            t->soloed = std::get<2>(e);
        }
    }
    gMeterSaved.clear();
    win->Unlock();
    win->PostMessage(kMsgUiRefresh);
}

static bool PlayMeterFixture(MainWindow* win, Project& project,
                             const std::string& wav, float gain,
                             TrackId* outTrack) {
    Track t;
    t.id = project.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "meter-fix";
    t.gain = gain;
    Clip c;
    c.id = project.NextClipId();
    c.startFrame = 0;
    c.lengthFrames = 48000;
    c.sourcePath = wav;
    t.clips.push_back(c);
    if (!LockedAddTrack(win, project, t)) return false;
    if (outTrack) *outTrack = t.id;
    // Silencing the rest is what isolates the meter -- but it is also state
    // the NEXT test inherits, and the tests after this one share the project:
    // every later render or playback then finds its material muted out. (That
    // leak went unnoticed for months and made a render-feedback fixture look
    // like it rendered impossibly fast -- its material was simply inaudible.)
    // What is changed is recorded here so RestoreMeterFixture undoes it
    // exactly, rather than assuming what the values were.
    if (win->LockWithTimeout(1000000) == B_OK) {
        gMeterSaved.clear();
        for (Track& other : project.Tracks()) {
            gMeterSaved.push_back({ other.id, other.muted, other.soloed });
            if (other.id == t.id) continue;
            other.soloed = false;
            other.muted  = true;
        }
        if (Track* mine = project.FindTrack(t.id)) mine->soloed = true;
        gMeterSaved.push_back({ t.id, false, true });   // what t holds now
        project.transport.playhead = 0;
        win->Unlock();
    }
    win->PostMessage(MSG_PLAY);
    return true;
}

static void TestMetering(MainWindow* win, Project& project, CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("master meter ...\n");

    // The view under test: the real one in the transport bar.
    MeterView* meter = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        meter = dynamic_cast<MeterView*>(win->FindView("meter"));
        win->Unlock();
    }
    CHECK(meter != nullptr);
    if (!meter) return;

    // The meter is on the transport bar, inside it, and inside the WINDOW.
    // That last clause is the one that bit: a pane re-layout widens the bar
    // (a plain BView's minimum is its current size), and everything pinned to
    // the bar's right edge was then placed past the window's edge -- painted
    // by nobody, over a background the server had already filled. A screenshot
    // review of the first cut found it; this is the cheap check of the same
    // thing, and the Pop out / dock toggle this test follows is what triggers
    // it (TestDockedEditor runs just before).
    {
        bool onBar = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            BView* bar = win->FindView("transport");
            const BRect onScreen = meter->ConvertToScreen(meter->Bounds());
            // (win->Frame() is shadowed by MainWindow's Frame typedef.)
            const BRect windowScreen = win->ConvertToScreen(win->Bounds());
            onBar = bar != nullptr && meter->Parent() == bar
                 && !meter->IsHidden()
                 && meter->Frame().right <= bar->Bounds().right
                 && meter->Frame().Width() > Themed(80.0f)
                 && onScreen.left >= windowScreen.left
                 && onScreen.right <= windowScreen.right;
            // ... and the bar itself fits the window: the root cause of the
            // whole thing is a pane whose minimum is its current size, which
            // made the window's layout wider than the window after any pane
            // re-layout (TimelineView says what it needs now).
            CHECK(bar != nullptr
                  && bar->Bounds().right <= win->Bounds().Width() + 1.0f);
            if (!onBar) {
                std::printf("  meter frame (%g,%g,%g,%g) bounds (%g,%g,%g,%g) "
                            "hidden %d parent %p\n",
                            meter->Frame().left, meter->Frame().top,
                            meter->Frame().right, meter->Frame().bottom,
                            meter->Bounds().left, meter->Bounds().top,
                            meter->Bounds().right, meter->Bounds().bottom,
                            (int)meter->IsHidden(), (void*)meter->Parent());
                if (bar)
                    std::printf("  bar bounds (%g,%g,%g,%g) scale %.2f\n",
                                bar->Bounds().left, bar->Bounds().top,
                                bar->Bounds().right, bar->Bounds().bottom,
                                ThemeScale());
            }
            win->Unlock();
        }
        CHECK(onBar);
    }

    // A 0.5-amplitude sine on both channels. Through the engine that is
    // 0.5 * 0.707 (the equal-power pan law's centre) = 0.354 peak, and a
    // sine's RMS is 0.354/sqrt(2) = 0.25.
    const std::string sine = WriteMeterWav("/tmp/haiku_daw_ui_meter.wav", 0.5,
                                           440.0);
    CHECK(!sine.empty());
    CHECK(PlayMeterFixture(win, project, sine, 1.0f, nullptr));
    CHECK(WaitFor([&] { return win->IsPlaying(); }, 5000000));
    CHECK(WaitFor([&] {
        return meter->Readings().rmsL > 0.2f;   // the window has fed it
    }, 8000000));
    // The RMS window is 300 ms long, so a reading taken while it fills is a
    // LOWER number: wait for it to settle before believing it.
    CHECK(WaitFor([&] {
        const float a = meter->Readings().rmsL;
        snooze(60000);
        const float b = meter->Readings().rmsL;
        return std::fabs(a - b) < 0.005f;
    }, 5000000));
    const MeterReadings r = meter->Readings();
    std::printf("  peak %.3f rms %.3f corr %+.2f\n", r.peakL, r.rmsL,
                r.correlation);
    CHECK(r.peakL > 0.30f && r.peakL < 0.40f);
    CHECK(r.rmsL > 0.22f && r.rmsL < 0.28f);      // 0.354/sqrt(2) = 0.25
    CHECK(r.rmsR > 0.22f && r.rmsR < 0.28f);
    CHECK(r.correlation > 0.9f);                  // mono: in phase
    CHECK(!meter->ClipLatched());                 // -3 dB is not a clip
    Shot("meter-playing");
    win->PostMessage(MSG_STOP);
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));

    // The peak hold remembers the peak the bars have already fallen from
    // (-9 dB, i.e. 0.77 of a 66 dB scale).
    CHECK(meter->HoldFracL() > 0.7f);

    // Full scale: the clip lamp latches, and a click on the meter clears it.
    // The fader is above unity so the mix really is over 0 dBFS (with the
    // 3 dB-centre pan law, unity alone would sit at -3 dBFS).
    const std::string full = WriteMeterWav("/tmp/haiku_daw_ui_meter_full.wav",
                                           1.0, 0.0);
    CHECK(!full.empty());
    CHECK(PlayMeterFixture(win, project, full, 1.5f, nullptr));
    CHECK(WaitFor([&] { return meter->ClipLatched(); }, 8000000));
    Shot("meter-clipped");
    win->PostMessage(MSG_STOP);
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));
    CHECK(meter->ClipLatched());      // stopping does not clear the lamp

    // The click: a real B_MOUSE_DOWN, posted to the view the way the app_server
    // delivers one ("be:view_where" is what BView::MessageReceived reads).
    {
        BMessage click(B_MOUSE_DOWN);
        click.AddInt32("buttons", 1);
        click.AddPoint("be:view_where", BPoint(6, 6));
        BMessenger(meter).SendMessage(&click);
        CHECK(WaitFor([&] { return !meter->ClipLatched(); }));
    }
    // ... and it stays clear on its own: the click cleared the window's latch
    // AND the engine's own flag bits, so the clip that already happened cannot
    // re-light it on the next poll.
    snooze(400000);
    CHECK(!meter->ClipLatched());

    // A fresh clip lights it again -- the lamp is not simply dead.
    win->PostMessage(MSG_PLAY);
    CHECK(WaitFor([&] { return win->IsPlaying(); }, 5000000));
    CHECK(WaitFor([&] { return meter->ClipLatched(); }, 8000000));
    // The engine's readings are what the meter drew: the correlation strip
    // shows the same number the view was handed (read while it is still
    // being fed -- stopping zeroes the bars, by design).
    CHECK(meter->Readings().correlation > 0.9f);
    win->PostMessage(MSG_STOP);
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));
    CHECK(meter->ClipLatched());   // the latch outlives the transport

    // Put the session back: the fixture muted every other track to isolate the
    // meter, and the tests after this one share the project. Leaving them
    // muted is a silent cross-test leak -- the material is there, it just does
    // not sound, which is exactly the kind of thing that reads as a fast
    // renderer or a broken engine somewhere else.
    win->PostMessage(MSG_STOP);
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));
    RestoreMeterFixture(win, project);
    CHECK(WaitQuiet());

    (void)stack;
}

// --- the dock's pages (T2) -------------------------------------------------

// Where segment `index` of the strip's segmented control sits, in the
// control's own coordinates. A click has to land INSIDE the segment whose page
// it means to ask for, and DawSegments lays its segments out from the label
// widths (+ Themed(16) each, scaled to the control's bounds), so the same
// computation is repeated here over the labels the window passes.
static BPoint SegmentClickPoint(BView* seg, int index) {
    static const char* const kLabels[] = { "Editor", "Samples", "Plugins" };
    const int count = (int)(sizeof(kLabels) / sizeof(kLabels[0]));
    float w[3] = { 0.0f, 0.0f, 0.0f };
    float total = 0.0f;
    for (int i = 0; i < count; i++) {
        w[i] = be_plain_font->StringWidth(kLabels[i]) + Themed(16.0f);
        total += w[i];
    }
    const BRect b = seg->Bounds();
    const float scale = total > 0.0f ? b.Width() / total : 1.0f;
    float x = b.left;
    for (int i = 0; i < index; i++) x += w[i] * scale;
    return BPoint(x + w[index] * scale * 0.5f, (b.top + b.bottom) * 0.5f);
}

// The colour a stock list (a "well") must carry in the ACTIVE mode: exactly
// what ApplyWellColors puts there -- the theme's document token in Dark mode,
// the system's own document colour in System.
static rgb_color ExpectedWellColor() {
    return ActiveThemeMode() == ThemeMode::Dark
        ? ColLcd() : ui_color(B_DOCUMENT_BACKGROUND_COLOR);
}

// One view's colour, read the way any view read from outside its window has to
// be (BView::ViewColor asks the server when the view holds a ui colour).
// `parent` is the browser view to look under, or null for the window itself.
static rgb_color ViewColorOf(MainWindow* win, const char* parent,
                             const char* name) {
    rgb_color c = B_TRANSPARENT_COLOR;
    if (win->LockWithTimeout(1000000) != B_OK) {
        std::printf("  (colour of %s: the window lock timed out)\n", name);
        return c;
    }
    BView* p = parent != nullptr ? win->FindView(parent) : nullptr;
    BView* v = p != nullptr ? p->FindView(name) : win->FindView(name);
    if (v != nullptr) c = v->ViewColor();
    else std::printf("  (colour of %s: view not found)\n", name);
    win->Unlock();
    return c;
}

// One colour check, with the values it saw: a bare == in a CHECK says nothing
// about which of the two was wrong.
static void CheckViewColor(MainWindow* win, const char* parent, const char* name,
                           rgb_color expected, const char* what, int line) {
    const rgb_color got = ViewColorOf(win, parent, name);
    ++g_checks;
    if (got == expected) return;
    ++g_fails;
    std::printf("  FAIL %s:%d  %s: %s is %d,%d,%d, expected %d,%d,%d\n",
                __FILE__, line, what, name, got.red, got.green, got.blue,
                expected.red, expected.green, expected.blue);
}

// --- 26. M3.1: the transport bar --------------------------------------------

// Every button posts its own action, the big display cycles its format and
// takes a typed position, and the readout line names the tempo, the meter, the
// callback load and the xrun count. Every click goes to the BAR at the rect
// the bar itself reports (`TransportButtonRect`), so a control that moved
// cannot still pass through an offset a test remembered (docs/HANDOFF.md).
static void TestTransportBar(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_transport_bar\n");

    // Something to play: an empty project stops playback again immediately,
    // which would read as "the Play button does nothing".
    {
        const char* wav = "/tmp/haiku_daw_ui_tbar.wav";
        std::remove(wav);
        WavWriter w;
        const int16_t frames[4] = { 0, 0, 3000, -3000 };
        CHECK(w.Open(wav, 48000, 2));
        CHECK(w.WriteInt16(frames, 4));
        CHECK(w.Close());
        Track t;
        t.id = project.NextTrackId();
        t.type = TrackType::Audio;
        t.name = "tbar-track";
        Clip c;
        c.id = project.NextClipId();
        c.startFrame = 0;
        c.lengthFrames = 48000 * 8;
        c.sourcePath = wav;
        t.clips.push_back(c);
        CHECK(LockedAddTrack(win, project, t));
    }

    BView* bar = nullptr;
    BView* timeView = nullptr;
    if (win->LockWithTimeout(1000000) == B_OK) {
        bar = win->TransportBarView();
        if (bar) timeView = bar->FindView("time");
        win->Unlock();
    }
    CHECK(bar != nullptr);
    CHECK(timeView != nullptr);
    Shot("transport-bar");

    auto lock = [&](const std::function<void()>& fn) {
        if (win->LockWithTimeout(1000000) == B_OK) { fn(); win->Unlock(); }
    };
    // A click the way the harness posts them: "where" in the view's own
    // coordinates AND "screen_where", which is what the window's dispatcher
    // hit-tests to decide which view gets the event (a message with only
    // "where" is delivered to nobody).
    auto click = [&](int which, int32 clicks = 1) {
        BRect r;
        lock([&] { r = win->TransportButtonRect(which); });
        const BPoint at((r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
        BPoint screen = at;
        lock([&] { screen = bar->ConvertToScreen(at); });
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", clicks);
            m.AddInt32("modifiers", 0);
            m.AddPoint("where", at);
            m.AddPoint("screen_where", screen);
            BMessenger(bar).SendMessage(&m);
        }
    };
    auto playing = [&] {
        bool p = false;
        lock([&] { p = win->IsPlaying(); });
        return p;
    };
    auto loopOn = [&] {
        bool on = false;
        lock([&] { on = project.transport.loopEnabled; });
        return on;
    };
    auto punchOn = [&] {
        bool on = false;
        lock([&] { on = project.transport.punchEnabled; });
        return on;
    };
    auto metronomeOn = [&] {
        bool on = false;
        lock([&] { on = win->MetronomeOn(); });
        return on;
    };
    auto playhead = [&] {
        Frame f = -1;
        lock([&] { f = project.transport.playhead; });
        return f;
    };
    auto setPlayhead = [&](Frame f) {
        lock([&] {
            project.transport.playhead = f;
            if (TimelineView* tv = dynamic_cast<TimelineView*>(win->FindView("timeline")))
                tv->SetPlayhead(f);
        });
    };

    const int kRtz = (int)TransportButton::Rtz;
    const int kRewind = (int)TransportButton::Rewind;
    const int kForward = (int)TransportButton::Forward;
    const int kPlay = (int)TransportButton::Play;
    const int kStop = (int)TransportButton::Stop;
    const int kLoop = (int)TransportButton::Loop;
    const int kClick = (int)TransportButton::Click;
    const int kPunch = (int)TransportButton::Punch;
    const int kZoomIn = (int)TransportButton::ZoomIn;

    // Play / Stop: the two the bar has always had, now through the same hit
    // test as the rest.
    CHECK(!playing());
    click(kPlay);
    CHECK(WaitFor([&] { return playing(); }));
    Shot("transport-bar-playing");
    click(kStop);
    CHECK(WaitFor([&] { return !playing(); }));

    // Loop, metronome and punch: each button flips the state it stands for,
    // and flips it back (the state is the model's/the controller's, not the
    // button's own copy).
    CHECK(!loopOn());
    click(kLoop);
    CHECK(WaitFor([&] { return loopOn(); }));
    click(kLoop);
    CHECK(WaitFor([&] { return !loopOn(); }));
    CHECK(!metronomeOn());
    click(kClick);
    CHECK(WaitFor([&] { return metronomeOn(); }));
    click(kClick);
    CHECK(WaitFor([&] { return !metronomeOn(); }));
    CHECK(!punchOn());
    click(kPunch);
    CHECK(WaitFor([&] { return punchOn(); }));
    click(kPunch);
    CHECK(WaitFor([&] { return !punchOn(); }));

    // Return to zero: back to frame 0 wherever the playhead was.
    setPlayhead(48000);
    CHECK(playhead() == 48000);
    click(kRtz);
    CHECK(WaitFor([&] { return playhead() == 0; }));

    // Rewind / fast-forward: one bar, on the tempo map's own boundaries. At
    // 120 BPM 4/4 and 48 kHz a bar is 96000 frames, so bar 3 starts at
    // 192000 and bar 4 at 288000.
    setPlayhead(288000);                       // in bar 4
    click(kRewind);
    CHECK(WaitFor([&] { return playhead() == 192000; }));   // bar 3's downbeat
    click(kForward);
    CHECK(WaitFor([&] { return playhead() == 288000; }));   // bar 4's
    setPlayhead(0);
    click(kRewind);                            // already at the first bar
    CHECK(WaitFor([&] { return playhead() == 0; }));

    // The zoom pair still zooms (it moved; its action did not).
    {
        double before = 0.0, after = 0.0;
        auto framesPerPixel = [&] {
            double fpp = 0.0;
            lock([&] {
                if (TimelineView* tv =
                        dynamic_cast<TimelineView*>(win->FindView("timeline"))) {
                    const float x0 = tv->FrameToX(0);
                    const float x1 = tv->FrameToX(48000);
                    if (x1 > x0) fpp = 48000.0 / (double)(x1 - x0);
                }
            });
            return fpp;
        };
        before = framesPerPixel();
        click(kZoomIn);
        CHECK(WaitFor([&] { after = framesPerPixel(); return after < before * 0.75; }));
    }

    // The big display: a click cycles the format, and what the bar shows is
    // what the window says the current format is (one piece of state).
    {
        int mode = -1;
        lock([&] { mode = win->TransportTimeMode(); });
        BMessage m(B_MOUSE_DOWN);
        m.AddInt32("buttons", 1);
        m.AddInt32("clicks", 1);
        m.AddPoint("where", BPoint(40, 20));   // inside the display's frame
        BMessenger(timeView).SendMessage(&m);
        CHECK(WaitFor([&] {
            int now = -1;
            lock([&] { now = win->TransportTimeMode(); });
            return now != mode;
        }));
        // ... and the tag under it names that format.
        std::string tag;
        lock([&] {
            if (auto* tv = dynamic_cast<TransportTimeView*>(timeView))
                tag = tv->TagText();
        });
        int now = -1;
        lock([&] { now = win->TransportTimeMode(); });
        CHECK(!tag.empty());
        CHECK(tag == TimeModeTag((TimeMode)now));

        // Cycle on to minutes:seconds, then type a position into the display
        // and check the playhead MOVED there.
        for (int guard = 0; guard < 5; guard++) {
            int m2 = -1;
            lock([&] { m2 = win->TransportTimeMode(); });
            if (m2 == (int)TimeMode::MinSec) break;
            BMessage c(B_MOUSE_DOWN);
            c.AddInt32("buttons", 1);
            c.AddInt32("clicks", 1);
            c.AddPoint("where", BPoint(40, 20));
            BMessenger(timeView).SendMessage(&c);
            snooze(120000);
        }
        int modeNow = -1;
        lock([&] { modeNow = win->TransportTimeMode(); });
        CHECK(modeNow == (int)TimeMode::MinSec);

        BMessage entry(kMsgTimeEntry);
        entry.AddString("text", "1:30");
        win->PostMessage(&entry);
        CHECK(WaitFor([&] { return playhead() == (Frame)(90 * 48000); }));

        // A position that cannot be read changes NOTHING and says so (a
        // plausible-but-wrong seek would be worse than the refusal).
        BMessage bad(kMsgTimeEntry);
        bad.AddString("text", "banana");
        win->PostMessage(&bad);
        snooze(400000);
        CHECK(playhead() == (Frame)(90 * 48000));
        std::string status;
        lock([&] {
            if (auto* b = dynamic_cast<TransportBar*>(bar)) status = b->StatusText();
        });
        CHECK(status.find("Cannot read") != std::string::npos);
    }

    // The readout line: tempo, meter, the callback's load and the xruns.
    {
        std::string info;
        lock([&] {
            if (auto* tv = dynamic_cast<TransportTimeView*>(timeView))
                info = tv->InfoText();
        });
        CHECK(info.find("BPM") != std::string::npos);
        CHECK(info.find("DSP") != std::string::npos);
        CHECK(info.find("XR") != std::string::npos);
    }

    // Leave the transport as the rest of the suite expects to find it.
    setPlayhead(0);
    CHECK(WaitQuiet());
}

// --- 26b. M4.4: a region job runs off the window thread ---------------------

// Reverse a real clip through the same message the timeline's clip menu posts:
// the decode happens on a worker thread with the progress bar up (the window
// keeps answering meanwhile), a cancel leaves the project EXACTLY as it was,
// and a completed run swaps the clip onto the rendered file.
static void TestRegionJob(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_region_job\n");

    // Long enough that the decode is a real piece of work: the cancel has to
    // land while the worker is still running for the check to mean anything.
    const char* srcPath = "/tmp/haiku_daw_ui_region_src.wav";
    std::remove(srcPath);
    {
        WavWriter w;
        CHECK(w.Open(srcPath, 48000, 2));
        // 20 s in 1 s blocks, so a failed write is visible rather than silent.
        std::vector<int16_t> block(48000 * 2);
        for (size_t i = 0; i < block.size(); i++)
            block[i] = (int16_t)((int)(i % 8000) - 4000);
        bool all = true;
        for (int s = 0; s < 20; s++) all = all && w.WriteInt16(block.data(), block.size());
        CHECK(all);
        CHECK(w.Close());
    }
    Track t;
    t.id = project.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "region-job";
    Clip c;
    c.id = project.NextClipId();
    c.startFrame = 0;
    c.lengthFrames = 48000 * 20;
    c.sourcePath = srcPath;
    t.clips.push_back(c);
    CHECK(LockedAddTrack(win, project, t));

    // The TRACK's clip, not the id the fixture made: the reverse op replaces
    // the clip with a fresh one (a new id) pointing at the rendered file, which
    // is exactly what the test is here to see.
    auto clipPath = [&] {
        std::string s;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* p = project.FindTrack(t.id))
                if (!p->clips.empty()) s = p->clips.front().sourcePath;
            win->Unlock();
        }
        return s;
    };
    auto postReverse = [&] {
        BMessage m(kMsgRegionReverse);
        m.AddInt64("track", (int64)t.id);
        m.AddInt64("clip", (int64)c.id);
        win->PostMessage(&m);
    };

    // --- cancel: the bar goes up, and the project is untouched ---
    {
        const int32 before = VisibleWindows();
        postReverse();
        const bool barUp = WaitFor([&] { return VisibleWindows() > before; },
                                   10000000);
        std::printf("  region job: the bar %s\n", barUp ? "came up" : "was missed");
        // The window keeps answering while the worker decodes: this post has to
        // be handled (the pulse runs, the looper is not blocked).
        CHECK(barUp);
        win->PostMessage(kMsgExportCancel);      // the bar's Cancel button
        CHECK(WaitFor([&] { return VisibleWindows() == before; }, 60000000));
        // Nothing changed: same path, and no reversed file anywhere near it.
        CHECK(clipPath() == srcPath);
        CHECK(WaitQuiet());
    }

    // --- and to completion: the clip now points at the rendered file ---
    {
        const int32 before = VisibleWindows();
        postReverse();
        // The bar APPEARS first, then it closes when the job is done. Waiting
        // for the close alone races: before the progress window is in the
        // app's window list "no extra window" is already true, so the wait
        // returns at once -- and a test that then deletes files would delete
        // them under a worker that is still reading.
        CHECK(WaitFor([&] { return VisibleWindows() > before; }, 10000000));
        CHECK(WaitFor([&] { return VisibleWindows() == before; }, 120000000));
        CHECK(WaitFor([&] { return clipPath() != srcPath; }, 10000000));
        const std::string now = clipPath();
        CHECK(!now.empty());
        CHECK(now != srcPath);
        CHECK(FileExists(now));
        // Put the original clip back for whatever runs after this, and take the
        // rendered file away -- never the source (a cleanup that fires on the
        // wrong branch is how the first version of this test broke itself).
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (Track* p = project.FindTrack(t.id))
                if (!p->clips.empty()) p->clips.front().sourcePath = srcPath;
            win->Unlock();
        }
        if (!now.empty() && now != srcPath) std::remove(now.c_str());
    }
    std::remove(srcPath);
    CHECK(WaitQuiet());
}

// --- 27. M3.4: the mixer strip's own edits ----------------------------------

// The strip's new intents reach the model through the command stack and come
// back in the strip's snapshot: the input trim, the polarity, the solo group
// and a send's level. The window is driven by the exact messages the strip
// posts (kMsgApplyMix / kMsgMixSend), which is also what the strip itself
// sends — the drawing and the drags are looked at in the screenshot pass.
static void TestMixerStripExtras(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_mixer_strip_extras\n");

    // An audio track with a send into a bus, so there is something to trim and
    // something to send to.
    Track bus;
    bus.id = project.NextTrackId();
    bus.type = TrackType::Bus;
    bus.name = "FX bus";
    CHECK(LockedAddTrack(win, project, bus));
    Track t;
    t.id = project.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "strip-track";
    t.sends.push_back(Send{ bus.id, 1.0f, false });
    CHECK(LockedAddTrack(win, project, t));

    win->PostMessage(MSG_MIXER_TEST);
    MixerStripsView* strips = nullptr;
    BWindow* mixer = nullptr;
    CHECK(WaitFor([&] {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (!w || w->IsHidden() || w == win) continue;
            if (w->LockWithTimeout(500000) != B_OK) continue;
            BView* v = w->FindView("strips");
            if (v) { mixer = w; strips = dynamic_cast<MixerStripsView*>(v); }
            w->Unlock();
            if (strips) return true;
        }
        return false;
    }));
    CHECK(strips != nullptr);
    if (strips == nullptr) return;

    // Asking for the mixer while it is open pushes a fresh strip payload and
    // raises the window. That payload now has ONE encoder; it used to have a
    // second, hand-rolled copy whose field names did not match what the mixer
    // decodes, so the strips came back nameless at unity gain on the second
    // View > Mixer. The name and the gain are what such a copy loses first.
    {
        win->PostMessage(MSG_MIXER_TEST);
        const bool kept = WaitFor([&] {
            if (mixer->LockWithTimeout(1000000) != B_OK) return false;
            bool found = false;
            for (const MixerStripInfo& s : strips->Strips())
                if (s.name == "strip-track" && s.gain > 0.9f && s.gain < 1.1f)
                    found = true;
            mixer->Unlock();
            return found;
        });
        CHECK(kept);
    }

    auto trackOf = [&] {
        Track out;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* p = project.FindTrack(t.id)) out = *p;
            win->Unlock();
        }
        return out;
    };

    // --- input trim: through the strip's own message ---
    {
        BMessage m(kMsgApplyMix);
        m.AddInt64("track", (int64)t.id);
        m.AddFloat("gain", 1.0f);
        m.AddFloat("pan", 0.0f);
        m.AddBool("mute", false);
        m.AddBool("solo", false);
        m.AddFloat("trim", 0.5f);
        m.AddBool("phase", false);
        m.AddInt32("sgroup", 0);
        win->PostMessage(&m);
        CHECK(WaitFor([&] { return trackOf().trim == 0.5f; }));
    }
    // --- polarity ---
    {
        BMessage m(kMsgApplyMix);
        m.AddInt64("track", (int64)t.id);
        m.AddFloat("gain", 1.0f);
        m.AddFloat("pan", 0.0f);
        m.AddBool("mute", false);
        m.AddBool("solo", false);
        m.AddFloat("trim", 0.5f);
        m.AddBool("phase", true);
        m.AddInt32("sgroup", 0);
        win->PostMessage(&m);
        CHECK(WaitFor([&] { return trackOf().phaseInvert; }));
    }
    // --- the solo group, and the cascade it implies ---
    {
        BMessage m(kMsgApplyMix);
        m.AddInt64("track", (int64)t.id);
        m.AddFloat("gain", 1.0f);
        m.AddFloat("pan", 0.0f);
        m.AddBool("mute", false);
        m.AddBool("solo", false);
        m.AddFloat("trim", 0.5f);
        m.AddBool("phase", true);
        m.AddInt32("sgroup", 2);
        win->PostMessage(&m);
        CHECK(WaitFor([&] { return trackOf().soloGroup == 2; }));

        // Put the bus in the same group and solo the audio track: the bus has
        // to come along (the group's whole point).
        {
            BMessage b(kMsgApplyMix);
            b.AddInt64("track", (int64)bus.id);
            b.AddFloat("gain", 1.0f);
            b.AddFloat("pan", 0.0f);
            b.AddBool("mute", false);
            b.AddBool("solo", false);
            b.AddFloat("trim", 1.0f);
            b.AddBool("phase", false);
            b.AddInt32("sgroup", 2);
            win->PostMessage(&b);
        }
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* bt = project.FindTrack(bus.id);
            const bool ok = bt && bt->soloGroup == 2;
            win->Unlock();
            return ok;
        }));
        {
            BMessage s(kMsgApplyMix);
            s.AddInt64("track", (int64)t.id);
            s.AddFloat("gain", 1.0f);
            s.AddFloat("pan", 0.0f);
            s.AddBool("mute", false);
            s.AddBool("solo", true);
            s.AddFloat("trim", 0.5f);
            s.AddBool("phase", true);
            s.AddInt32("sgroup", 2);
            win->PostMessage(&s);
        }
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* bt = project.FindTrack(bus.id);
            const Track* at = project.FindTrack(t.id);
            const bool ok = bt && at && bt->soloed && at->soloed;
            win->Unlock();
            return ok;
        }));
        // Leave it unsoloed again for the tests after this one.
        BMessage s2(kMsgApplyMix);
        s2.AddInt64("track", (int64)t.id);
        s2.AddFloat("gain", 1.0f);
        s2.AddFloat("pan", 0.0f);
        s2.AddBool("mute", false);
        s2.AddBool("solo", false);
        s2.AddFloat("trim", 0.5f);
        s2.AddBool("phase", true);
        s2.AddInt32("sgroup", 2);
        win->PostMessage(&s2);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* bt = project.FindTrack(bus.id);
            const bool ok = bt && !bt->soloed;
            win->Unlock();
            return ok;
        }));
    }
    // --- a send's level: the knob's own message ---
    {
        BMessage m(kMsgMixSend);
        m.AddInt64("track", (int64)t.id);
        m.AddInt32("index", 0);
        m.AddFloat("level", 0.25f);
        win->PostMessage(&m);
        CHECK(WaitFor([&] {
            const Track tr = trackOf();
            return tr.sends.size() == 1
                && std::fabs(tr.sends[0].level - 0.25f) < 1e-4f;
        }));
    }

    // ... and the strip's own snapshot carries all of it after a refresh.
    win->PostMessage(kMsgUiRefresh);
    CHECK(WaitFor([&] {
        bool ok = false;
        if (mixer->LockWithTimeout(1000000) == B_OK) {
            for (const MixerStripInfo& s : strips->Strips())
                if (s.trackId == t.id)
                    ok = std::fabs(s.trim - 0.5f) < 1e-4f && s.phaseInvert
                      && s.soloGroup == 2 && s.sends.size() == 1
                      && std::fabs(s.sends[0].level - 0.25f) < 1e-4f;
            mixer->Unlock();
        }
        return ok;
    }));
    Shot("mixer-strips");

    // Back to the model's defaults for whatever runs next.
    {
        BMessage m(kMsgApplyMix);
        m.AddInt64("track", (int64)t.id);
        m.AddFloat("gain", 1.0f);
        m.AddFloat("pan", 0.0f);
        m.AddBool("mute", false);
        m.AddBool("solo", false);
        m.AddFloat("trim", 1.0f);
        m.AddBool("phase", false);
        m.AddInt32("sgroup", 0);
        win->PostMessage(&m);
        CHECK(WaitFor([&] { return trackOf().trim == 1.0f; }));
    }
    HideOtherWindows(win);
    CHECK(WaitQuiet());
}

// The dock hosts one page at a time, switched from the strip's segmented
// control: the editor (the roll or its hint), the samples and the plugins. The
// browsers are the SAME views their windows host, so what is checked here is
// that the right one is in the body, that the strip says so, and that the
// controls in the pages reach their own view (T2 review: the pages' widgets
// posted to the window, and the switch posted a page number nobody sent).
static void TestDockPages(MainWindow* win, Project& project, CommandStack* stack) {
    std::printf("dock pages ...\n");

    // The window is normalised first: an earlier test drives the 150% layout,
    // which makes the window GROW to satisfy its minimum (Haiku's layout
    // enforces it), and a window wider than the screen is what hid the strip's
    // own buttons in the first place.
    if (win->Lock()) {
        win->ResizeTo(900.0f, 640.0f);
        win->Unlock();
        win->InvalidateLayout(true);   // the layout pass a drag would cause
    }
    snooze(250000);

    BView* dock = nullptr;
    win->Lock();
    dock = win->FindView("dock");
    win->Unlock();
    if (dock == nullptr) {   // the previous test popped the dock away: bring it back
        win->PostMessage(MSG_TOGGLE_DOCK);
        snooze(200000);
    }

    // The real switch: a click on the strip's segmented control, whose
    // MouseDown picks the segment under the pointer and Invokes the page it
    // selected. The old test posted MSG_DOCK_PAGE with its own hand-written
    // "index", which is exactly how a control that never sent one passed.
    DawSegments* segments = nullptr;
    win->Lock();
    segments = dynamic_cast<DawSegments*>(win->FindView("dockpages"));
    win->Unlock();
    CHECK(segments != nullptr);
    if (segments == nullptr) return;

    auto clickPage = [&](int index) {
        BPoint at, screen;
        bool shown = false;
        win->Lock();
        // A detached view has no owner to convert coordinates with -- and one
        // is detached exactly when the dock is hidden. Say so and fail, rather
        // than let the kit trap in ConvertToScreen (which takes the whole test
        // process, and the VM, down with it).
        shown = segments->Window() != nullptr;
        if (shown) {
            at = SegmentClickPoint(segments, index);
            screen = segments->ConvertToScreen(at);
        }
        win->Unlock();
        CHECK(shown);   // the dock must be showing for a click to land
        if (!shown) return;
        const uint32 whats[2] = { B_MOUSE_DOWN, B_MOUSE_UP };
        for (uint32 what : whats) {
            BMessage m(what);
            m.AddInt32("buttons", what == B_MOUSE_DOWN ? 1 : 0);
            m.AddInt32("clicks", 1);
            m.AddPoint("where", at);
            m.AddPoint("screen_where", screen);
            BMessenger(segments).SendMessage(&m);
        }
    };
    auto onPage = [&](int page) {
        int32 sel = -1;
        if (win->LockWithTimeout(1000000) == B_OK) {
            sel = segments->Selected();
            win->Unlock();
        }
        return sel == page;
    };

    // Samples: the middle segment. The page's view is in the body, the other
    // pages are not, and the control agrees about which one is up -- the last
    // of those is what a control that Invoked without a page number failed.
    clickPage(1);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = win->FindView("samplebrowserview") != nullptr
                     && win->FindView("pluginbrowserview") == nullptr
                     && win->FindView("roll") == nullptr;
        win->Unlock();
        return ok;
    }));
    CHECK(onPage(1));
    // And themed for the mode this pass runs in, at the moment it is attached:
    // the panel colour and the list's well (T2 review).
    CheckViewColor(win, nullptr, "samplebrowserview", ColHeader(),
                   "the samples page panel", __LINE__);
    CheckViewColor(win, "samplebrowserview", "list", ExpectedWellColor(),
                   "the samples list well", __LINE__);
    Shot("dock-samples");

    // Plugins, the last segment.
    clickPage(2);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = win->FindView("pluginbrowserview") != nullptr
                     && win->FindView("samplebrowserview") == nullptr;
        win->Unlock();
        return ok;
    }));
    CHECK(onPage(2));
    CheckViewColor(win, nullptr, "pluginbrowserview", ColHeader(),
                   "the plugins page panel", __LINE__);
    CheckViewColor(win, "pluginbrowserview", "list", ExpectedWellColor(),
                   "the plugins list well", __LINE__);
    Shot("dock-plugins");

    // The page with NO track selected (the UX-shell pass): it cannot add an
    // insert to a chain nobody named, so it must SAY so -- the title asks for a
    // selection ("add to no track selected" read as an error about a chain the
    // user never had), and a pick answers on the status line instead of being
    // swallowed in silence.
    {
        auto dockTitle = [&] {
            std::string s;
            if (win->LockWithTimeout(1000000) == B_OK) {
                if (BStringView* t = dynamic_cast<BStringView*>(
                        win->FindView("edtitle")))
                    s = t->Text() ? t->Text() : "";
                win->Unlock();
            }
            return s;
        };
        BMessage none(kMsgTrackSelected);
        none.AddInt64("track", 0);          // kInvalidTrackId: nothing selected
        win->PostMessage(&none);
        CHECK(WaitFor([&] {
            return dockTitle().find("select a track") != std::string::npos;
        }));
        std::printf("  plugins page with no track: \"%s\"\n",
                    dockTitle().c_str());

        BView* v = nullptr;
        win->Lock();
        v = win->FindView("pluginbrowserview");
        win->Unlock();
        CHECK(v != nullptr);
        if (v != nullptr) {
            win->Lock();
            if (BListView* l = dynamic_cast<BListView*>(v->FindView("list")))
                l->Invoke();   // the LIST's own invocation, a double-click
            win->Unlock();
        }
        CHECK(WaitFor([&] {
            return TransportStatus(win) == "Select a track first";
        }, 5000000));

        // Back to a real selection for the rest of the test (the pick below
        // selects its own track; this keeps the page honest in between).
        TrackId back = kInvalidTrackId;
        win->Lock();
        if (!project.Tracks().empty()) back = project.Tracks().front().id;
        win->Unlock();
        BMessage sel(kMsgTrackSelected);
        sel.AddInt64("track", (int64)back);
        win->PostMessage(&sel);
        snooze(200000);
    }

    // The strip's own controls belong ON the strip: a title with an unlimited
    // max size, next to a control that stretches, once pushed Pop out and the x
    // past the window's edge -- a layout can be perfectly "valid" and still
    // hide the buttons a user needs.
    {
        win->Lock();
        float closeRight = -1.0f, stripRight = -1.0f, popLeft = -1.0f;
        if (BView* strip = win->FindView("dockhead"))
            stripRight = strip->Bounds().right;
        if (BView* close = win->FindView("dockclose"))
            closeRight = close->Frame().right;
        if (BView* pop = win->FindView("popout"))
            popLeft = pop->Frame().left;
        win->Unlock();
        // The strip spans the window it lives in: a dock that keeps an older,
        // wider width hangs its own buttons off the window's edge.
        float winW = 0.0f;
        win->Lock();
        winW = win->Bounds().Width();
        win->Unlock();
        std::printf("  window %.0f wide, strip right %.0f\n", winW, stripRight);
        win->Lock();
        win->Unlock();
        CHECK(stripRight <= winW + 1.0f);
        std::printf("  window %.0f wide: strip right %.0f, popout at %.0f, "
                    "close right %.0f\n", winW, stripRight, popLeft, closeRight);
        win->Lock();
        auto dump = [&](const char* n) {
            if (BView* v = win->FindView(n))
                std::printf("    %s frame %.0f,%.0f,%.0f,%.0f min %.0f\n", n,
                            v->Frame().left, v->Frame().top, v->Frame().right,
                            v->Frame().bottom, v->MinSize().width);
            else std::printf("    %s: not found\n", n);
        };
        dump("dock"); dump("dockhead"); dump("pluginbrowserview"); dump("list");
        win->Unlock();
        CHECK(closeRight > 0.0f && stripRight > 0.0f && closeRight <= stripRight);
        CHECK(popLeft > 0.0f && popLeft < closeRight);
    }

    // Back to the editor page (the first segment): the hint, since no region
    // is open.
    clickPage(0);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = win->FindView("samplebrowserview") == nullptr
                     && win->FindView("pluginbrowserview") == nullptr
                     && win->FindView("roll") == nullptr;
        win->Unlock();
        return ok;
    }));
    CHECK(onPage(0));
    Shot("dock-editor-page");

    // The strip's title and the empty-state hint follow a mode switch: a
    // BStringView copies its parent's view colour when it is ATTACHED
    // (StringView.cpp), so a switch left them as boxes in the mode they were
    // built in -- in the dark mode the title drew light text on its own light
    // box (the page's name was unreadable), which the dark-pass shots caught.
    {
        const ThemeMode runMode = ActiveThemeMode();
        win->PostMessage(MSG_THEME_MODE);
        CHECK(WaitFor([&] { return ActiveThemeMode() != runMode; }));
        CheckViewColor(win, "dockhead", "edtitle", ColHeader(),
                       "the strip's title", __LINE__);
        CheckViewColor(win, "dockempty", "dockhint", ColBackground(),
                       "the empty-state hint", __LINE__);
        win->PostMessage(MSG_THEME_MODE);
        CHECK(WaitFor([&] { return ActiveThemeMode() == runMode; }));
        CheckViewColor(win, "dockhead", "edtitle", ColHeader(),
                       "the strip's title back in the pass mode", __LINE__);
        CheckViewColor(win, "dockempty", "dockhint", ColBackground(),
                       "the empty-state hint back in the pass mode", __LINE__);
    }

    // --- the sample browser's own messages ---------------------------------
    //
    // The field and the list belong to the browser VIEW now; left untargeted,
    // their messages go to the window that hosts them (BControl/BListView set
    // that target on attach), and neither MainWindow nor the browser window
    // handles 'srch'/'pick'. So this drives the widgets themselves: the field's
    // Invoke (what Enter in it does) and the list's Invoke (what a double-click
    // does), not the message ids.
    const std::string fixture = BrowseFixturePath();
    CHECK(!fixture.empty());
    BEntry(fixture.c_str()).Remove();   // a leftover from an earlier run

    clickPage(1);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = win->FindView("samplebrowserview") != nullptr;
        win->Unlock();
        return ok;
    }));

    auto sampleView = [&]() -> BView* {
        BView* v = nullptr;
        if (win->LockWithTimeout(1000000) == B_OK) {
            v = win->FindView("samplebrowserview");
            win->Unlock();
        }
        return v;
    };
    auto sampleRows = [&] {
        int32 n = -1;
        if (win->LockWithTimeout(1000000) != B_OK) return n;
        if (BView* v = win->FindView("samplebrowserview"))
            if (BListView* l = dynamic_cast<BListView*>(v->FindView("list")))
                n = l->CountItems();
        win->Unlock();
        return n;
    };
    auto sampleSearch = [&](const char* text) {
        BView* v = sampleView();
        if (v == nullptr) return;
        win->Lock();
        if (BTextControl* field =
                dynamic_cast<BTextControl*>(v->FindView("filter"))) {
            field->SetText(text);
            field->Invoke();   // what pressing Enter in the field does
        }
        win->Unlock();
    };

    // The filter the fixture WILL match — but it is not written yet, so the
    // search finds nothing: that is also the state (an empty list) the old
    // "query while the list is empty" test used to re-query from, so the next
    // step can tell the two apart.
    sampleSearch("haiku-daw-ui-browse");
    CHECK(WaitFor([&] { return sampleRows() == 0; }));

    // The query runs ONCE, on the first showing of the page — not again on
    // every page switch (the page is re-attached on each one). Proof: a file
    // that appears while the page is away is NOT found by switching back to it,
    // even though the filter would now match it, because no query runs; asking
    // again with the field does find it.
    clickPage(0);
    snooze(150000);
    CHECK(WriteSilentWav(fixture));
    clickPage(1);
    snooze(400000);                        // long enough for a query to finish
    CHECK(sampleRows() == 0);              // no re-query on attach
    sampleSearch("haiku-daw-ui-browse");   // ask: the query finds it now
    CHECK(WaitFor([&] { return sampleRows() == 1; }, 20000000));

    // And the list's invocation imports the file the way a double-click does:
    // its own Invoke, through the target the view sets on attach.
    {
        size_t before = 0;
        if (win->LockWithTimeout(1000000) == B_OK) {
            before = CountClips(project);
            win->Unlock();
        }
        BView* v = sampleView();
        bool themedRows = false;
        win->Lock();
        if (v != nullptr)
            if (BListView* l = dynamic_cast<BListView*>(v->FindView("list"))) {
                // The rows are drawn in the THEME's text colour: BListView::
                // DrawItem uses the system colour table, which the app may not
                // rewrite, so plain BStringItems were black on the dark well
                // (the dark-pass shots). The class is what the pixels follow.
                themedRows = l->CountItems() > 0
                          && dynamic_cast<DawStringItem*>(l->ItemAt(0)) != nullptr;
                l->Select(0);      // what the click before the double-click does
                l->Invoke();
            }
        win->Unlock();
        CHECK(themedRows);
        CHECK(WaitFor([&] {
            bool ok = false;
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            ok = CountClips(project) == before + 1
              && ProjectHasClip(project, "haiku-daw-ui-browse.wav");
            win->Unlock();
            return ok;
        }, 20000000));
    }
    BEntry(fixture.c_str()).Remove();   // leave the settings dir as it was

    // Pop out: the showing page becomes its own window — the browser WINDOW
    // hosts the same view, and its theming was the split's victim too, so it
    // gets looked at (nothing else in the suite opens it). The dock goes back
    // to the editor page and hides itself, as its own Pop out does.
    {
        const int before = VisibleWindows();
        win->PostMessage(MSG_POP_OUT_EDITOR);   // the strip's Pop out
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(300000);
        Shot("browser-window-popped");
        // Close it again: it is the one visible window that is not the main one.
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w == nullptr || w == win || w->IsHidden()) continue;
            if (w->LockWithTimeout(1000000) == B_OK) w->Quit();
            break;
        }
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool gone = win->FindView("dock") == nullptr;   // hidden, as
            win->Unlock();                                        // Pop out does
            return gone;
        }));
    }


    // A choice made in the DOCKED plugin browser runs the same path the window
    // does: the inspector turns it into a SetFxCommand against the track named
    // in the message, which is what makes the docked page worth having.
    Track t = MakeMidiTrack(project, {}, "dock-fx");
    CHECK(LockedAddTrack(win, project, t));
    BMessage select(kMsgTrackSelected);
    select.AddInt64("track", (int64)t.id);
    win->PostMessage(&select);          // the page follows the selection
    snooze(100000);

    BMessage addFx(kMsgMixFxAdd);
    addFx.AddInt64("track", (int64)t.id);
    win->PostMessage(&addFx);           // what the empty slot in the inspector sends
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = win->FindView("pluginbrowserview") != nullptr;
        win->Unlock();
        return ok;
    }));

    {
        int32 chainBefore = 0;
        win->Lock();
        if (Track* mt = project.FindTrack(t.id)) chainBefore = (int32)mt->fx.size();
        win->Unlock();
        CHECK(chainBefore == 0);

        auto browser = [&]() -> BView* {
            BView* v = nullptr;
            if (win->LockWithTimeout(1000000) == B_OK) {
                v = win->FindView("pluginbrowserview");
                win->Unlock();
            }
            return v;
        };
        auto pluginRows = [&] {
            int32 n = -1;
            if (win->LockWithTimeout(1000000) != B_OK) return n;
            if (BView* v = win->FindView("pluginbrowserview"))
                if (BListView* l = dynamic_cast<BListView*>(v->FindView("list")))
                    n = l->CountItems();
            win->Unlock();
            return n;
        };
        const int32 allRows = pluginRows();
        CHECK(allRows > 1);

        // Typing in the page's filter: the field's own invoke, which is what
        // Enter in it does. The list narrows through the VIEW (T2 review).
        {
            BView* v = browser();
            win->Lock();
            if (v != nullptr)
                if (BTextControl* field =
                        dynamic_cast<BTextControl*>(v->FindView("filter"))) {
                    field->SetText("widener");
                    field->Invoke();   // MSG_FILTER -> Rebuild()
                }
            win->Unlock();
        }
        CHECK(WaitFor([&] {
            const int32 n = pluginRows();
            return n >= 1 && n < allRows;   // narrowed to the matches
        }));

        // Pick the first row: the LIST's own invocation, which is what a
        // double-click fires and therefore goes wherever the list's target
        // points. The old test posted 'pbpk' to the view directly, which says
        // nothing about the target -- and that is exactly how a browser whose
        // filter and pick were both dead passed (T2 review).
        {
            BView* v = browser();
            win->Lock();
            if (v != nullptr)
                if (BListView* l = dynamic_cast<BListView*>(v->FindView("list")))
                    l->Invoke();   // Rebuild() selected row 0
            win->Unlock();
        }
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            Track* mt = project.FindTrack(t.id);
            const bool ok = mt != nullptr && mt->fx.size() == 1;
            win->Unlock();
            return ok;
        }));
        Shot("dock-plugin-chosen");
    }

    // A closed dock stays closed when the selection moves: the page follows the
    // selection (its title names the new chain) without being SHOWN, because a
    // page switch that showed the dock would re-open it every time a track is
    // clicked -- and right after a restart, where the restored page is remembered
    // while the dock is down (T2 review). The mixer's strip click is the other
    // half of that point: it moves the inspector AND the page now.
    {
        // An EXISTING track: a test should not grow the project the later
        // timing test measures (it did, by three tracks' worth).
        TrackId other = kInvalidTrackId;
        std::string otherName;
        win->Lock();
        for (const Track& tr : project.Tracks()) {
            if (tr.id == t.id) continue;
            other = tr.id; otherName = tr.name;
            break;
        }
        win->Unlock();
        CHECK(other != kInvalidTrackId && !otherName.empty());

        win->PostMessage(MSG_TOGGLE_DOCK);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool gone = win->FindView("dock") == nullptr;
            win->Unlock();
            return gone;
        }));
        BMessage mixSelect(kMsgMixSelect);
        mixSelect.AddInt64("track", (int64)other);
        win->PostMessage(&mixSelect);
        snooze(250000);
        bool hidden = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            hidden = win->FindView("dock") == nullptr;
            win->Unlock();
        }
        CHECK(hidden);                 // the selection did not re-open it

        win->PostMessage(MSG_TOGGLE_DOCK);   // back up for what follows
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->FindView("dock") != nullptr;
            win->Unlock();
            return ok;
        }));
        // ... and while it was hidden the page was re-pointed, not left stale:
        // the strip names the chain the mixer's strip click selected.
        std::string title;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (BStringView* tv =
                    dynamic_cast<BStringView*>(win->FindView("edtitle")))
                title = tv->Text();
            win->Unlock();
        }
        CHECK(title.find(otherName) != std::string::npos);
    }

    // And the same chain opens the single effects window, retargeted.
    {
        BMessage show(kMsgShowFx);
        show.AddInt64("track", (int64)t.id);
        show.AddInt32("focus", 0);
        const int before = VisibleWindows();
        win->PostMessage(&show);
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(300000);
        Shot("fx-window-track");

        // Opening it again -- for another track -- does NOT make a second one.
        Track t2 = MakeMidiTrack(project, {}, "dock-fx-2");
        CHECK(LockedAddTrack(win, project, t2));
        BMessage show2(kMsgShowFx);
        show2.AddInt64("track", (int64)t2.id);
        show2.AddInt32("focus", -1);
        win->PostMessage(&show2);
        CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
        snooze(300000);
        // The title names the chain that is in it now: "<track>" alone (whole
        // chain) rather than the first track's name.
        bool titled = false;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w == nullptr || w->IsHidden()) continue;
            const char* name = WindowTitle(w);
            if (name != nullptr && std::strstr(name, "dock-fx-2") != nullptr)
                titled = true;
        }
        CHECK(titled);
        Shot("fx-window-retargeted");

        // Close it again, so the rest of the run sees the window count it
        // expects (and the app keeps only the main window).
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w == nullptr || w->IsHidden()) continue;
            const char* name = WindowTitle(w);
            if (name != nullptr && std::strstr(name, "dock-fx") != nullptr) {
                if (w->Lock()) { w->Quit(); }
                break;
            }
        }
        CHECK(WaitFor([&] { return VisibleWindows() == before; }));
    }

    // A region opened while a BROWSER page is showing: the docked roll takes
    // the body, it does not land beside the browser. Adding it without
    // switching pages left TWO children in the body (they split the pane) while
    // the strip still named the browser (T2 review).
    {
        clickPage(1);   // Samples up
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->FindView("samplebrowserview") != nullptr;
            win->Unlock();
            return ok;
        }));
        BMessage open(kMsgOpenEditor);
        open.AddInt64("track", (int64)t.id);
        open.AddInt64("clip", (int64)t.midiClips.front().id);
        win->PostMessage(&open);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->FindView("roll") != nullptr
                         && win->FindView("samplebrowserview") == nullptr
                         && win->FindView("pluginbrowserview") == nullptr;
            win->Unlock();
            return ok;
        }));
        CHECK(onPage(0));
        Shot("dock-editor-over-browser");
    }

    // --- the restored page (T2 review) --------------------------------------
    //
    // LoadSettings restored the page NUMBER and the segment highlight but never
    // put the page in the body: a restart with Samples or Plugins showing left
    // the strip naming one page and the body showing another, and the
    // highlighted segment could not even be clicked back into agreement (a
    // click on the current segment is a no-op). ReloadSettings is the restore a
    // restart runs, and the settings file is what it reads.
    {
        float dockH = 260.0f;
        win->Lock();
        if (BView* d = win->FindView("dock")) dockH = d->Bounds().Height();
        win->Unlock();
        win->PostMessage(MSG_TOGGLE_DOCK);   // hidden: the state the review named
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool gone = win->FindView("dock") == nullptr;
            win->Unlock();
            return gone;
        }));

        AppSettings s;
        s.dockPage = 2;                 // Plugins
        s.bottomVisible = true;
        s.inspectorVisible = true;
        s.bottomHeight = dockH;
        if (win->LockWithTimeout(1000000) == B_OK) {
            const BRect fr = win->BWindow::Frame();   // MainWindow::Frame is a typedef
            s.winL = fr.left; s.winT = fr.top;
            s.winR = fr.right; s.winB = fr.bottom;
            win->Unlock();
        }
        BPath sp;
        CHECK(ProjectDocument::SettingsPath(sp));
        {
            BFile f(sp.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
            CHECK(f.InitCheck() == B_OK);
            const std::string text = s.Serialize();
            if (f.InitCheck() == B_OK) f.Write(text.data(), text.size());
        }

        win->Lock();
        win->ReloadSettings();
        win->Unlock();
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->FindView("dock") != nullptr
                         && win->FindView("pluginbrowserview") != nullptr
                         && win->FindView("samplebrowserview") == nullptr
                         && win->FindView("roll") == nullptr;
            win->Unlock();
            return ok;
        }));
        CHECK(onPage(2));
        Shot("dock-restored-page");
    }

    // --- the pages and a mode switch (T2 review) ----------------------------
    //
    // Every page view takes the mode it is created in (its own attach) and the
    // one it is switched to (the walk). The old split lost both: the browser
    // views cached the constructor's panel colour, had no attach-time theme at
    // all, and a page that was detached when the mode changed was never walked.
    {
        const ThemeMode runMode = ActiveThemeMode();
        win->PostMessage(MSG_THEME_MODE);   // the OTHER mode
        CHECK(WaitFor([&] { return ActiveThemeMode() != runMode; }));
        // The strip's own labels: a BStringView copies its parent's view colour
        // when it is ATTACHED (StringView.cpp), so a mode switch left the title
        // and the empty-state hint as boxes in the mode they were built in --
        // in the dark mode the title drew light text on its own light box and
        // the page's name was unreadable (caught in the dark-pass shots). The
        // hint's own check runs where it is on screen (the editor page).
        CheckViewColor(win, "dockhead", "edtitle", ColHeader(),
                       "the strip's title", __LINE__);
        // Plugins is up: the walk reaches it.
        CheckViewColor(win, nullptr, "pluginbrowserview", ColHeader(),
                       "the plugins panel after the switch", __LINE__);
        CheckViewColor(win, "pluginbrowserview", "list", ExpectedWellColor(),
                       "the plugins well after the switch", __LINE__);
        // Samples is detached: it takes the mode when it comes BACK (its own
        // attach), not when something happens to walk the tree.
        clickPage(1);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = win->FindView("samplebrowserview") != nullptr;
            win->Unlock();
            return ok;
        }));
        CheckViewColor(win, nullptr, "samplebrowserview", ColHeader(),
                       "the re-attached samples panel", __LINE__);
        CheckViewColor(win, "samplebrowserview", "list", ExpectedWellColor(),
                       "the re-attached samples well", __LINE__);
        Shot("dock-theme-other");

        win->PostMessage(MSG_THEME_MODE);   // back to the pass's own mode
        CHECK(WaitFor([&] { return ActiveThemeMode() == runMode; }));
        CheckViewColor(win, nullptr, "samplebrowserview", ColHeader(),
                       "the samples panel back in the pass mode", __LINE__);
        CheckViewColor(win, "samplebrowserview", "list", ExpectedWellColor(),
                       "the samples well back in the pass mode", __LINE__);
    }

    // The page switch must not become a key trap: Space is the transport's,
    // whatever control holds the focus (a text field being typed into is the
    // one exception). The click above focused the segment control, whose own
    // BControl::KeyDown ate Space -- after a page click, play/stop did nothing
    // at all.
    {
        BMessage key(B_KEY_DOWN);
        key.AddString("bytes", " ");
        key.AddInt32("modifiers", 0);
        win->PostMessage(&key);
        CHECK(WaitFor([&] { return win->IsPlaying(); }, 10000000));
        win->PostMessage(&key);
        CHECK(WaitFor([&] { return !win->IsPlaying(); }, 10000000));
    }

    // The dock goes back to the editor page for whatever runs next.
    clickPage(0);
    snooze(150000);
}

// --- the effects panel's retarget (T2 review) -------------------------------

// The single effects window, retargeted at another chain while the panel has an
// edit in flight. What the user already HEARD must reach the MODEL before the
// view is pointed elsewhere: a drag previews live (kMsgFxLive -> the engine)
// and commits on mouse-up, and the retarget used to DROP that preview, leaving
// the project -- and the file -- on the old value while the audio played the
// new one until the next rebuild reverted it.
//
// Driven through the REAL window: the opener, the retarget message and the
// model commit are the app's own, and the assertion is on the model. The drag
// needs the panel to have been DRAWN (its hit regions are rebuilt by Draw), so
// the window gets a moment before the click.
static void TestFxRetargetFlush(MainWindow* win, Project& project) {
    std::printf("effects retarget flush ...\n");

    // EXISTING tracks: a test should not grow the project the later timing test
    // measures. The first one gets a single Limiter (one panel, its knob row at
    // the top); the second is where the window is retargeted to.
    TrackId a = kInvalidTrackId, b = kInvalidTrackId;
    std::string bName;
    win->Lock();
    for (Track& tr : project.Tracks())          // prefer the one the dock test made
        if (a == kInvalidTrackId && tr.name == "dock-fx") a = tr.id;
    if (a == kInvalidTrackId)
        for (const Track& tr : project.Tracks())
            if (a == kInvalidTrackId) a = tr.id;
    for (Track& tr : project.Tracks()) {
        if (tr.id == a) {
            tr.fx.clear();
            tr.fx.push_back(MakeInsertDesc(EffectType::Limiter, ""));
        } else if (b == kInvalidTrackId) {
            b = tr.id; bName = tr.name;
        }
    }
    win->Unlock();
    CHECK(a != kInvalidTrackId && b != kInvalidTrackId && !bName.empty());
    if (a == kInvalidTrackId || b == kInvalidTrackId) return;
    win->PostMessage(kMsgUiRefresh);

    const int before = VisibleWindows();
    BMessage show(kMsgShowFx);
    show.AddInt64("track", (int64)a);
    show.AddInt32("focus", 0);       // one panel: its knob row is at the top
    win->PostMessage(&show);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
    snooze(400000);                  // let it draw: fHits comes from Draw

    auto fxWindow = [&]() -> BWindow* {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w == nullptr || w == win || w->IsHidden()) continue;
            if (std::strstr(WindowTitle(w), "Limiter") != nullptr) return w;
        }
        return nullptr;
    };
    BWindow* fw = fxWindow();
    CHECK(fw != nullptr);
    if (fw == nullptr) return;

    BView* view = nullptr;
    if (fw->LockWithTimeout(1000000) == B_OK) {
        view = fw->FindView("fx");       // the EffectsView
        fw->Unlock();
    }
    CHECK(view != nullptr);
    if (view == nullptr) return;

    // A knob drag: press inside the first knob's cell and pull up. One panel
    // sits at kPanelPad with a title row and a header row above the knobs
    // (22 + 22 + 4); the first cell is 68 design px wide from +8. A generous
    // point in the middle of it — THEMED, because the panel's own geometry is:
    // the run's UI scale is the user preference the settings file carries (and
    // the Preferences window can change it live), so a point in design pixels
    // stops being the middle of that cell as soon as the scale is not 100%.
    const BPoint from(Themed(50.0f), Themed(100.0f));
    const BPoint to(Themed(50.0f), Themed(60.0f));
    BPoint fromScreen = from, toScreen = to;
    if (fw->LockWithTimeout(1000000) == B_OK) {
        fromScreen = view->ConvertToScreen(from);
        toScreen   = view->ConvertToScreen(to);
        fw->Unlock();
    }
    auto postMouse = [&](uint32 what, BPoint where, BPoint screen) {
        BMessage m(what);
        m.AddInt32("buttons", what == B_MOUSE_UP ? 0 : 1);
        m.AddInt32("clicks", 1);
        m.AddPoint("where", where);
        m.AddPoint("screen_where", screen);
        BMessenger(view).SendMessage(&m);
    };
    // The value the engine is playing (the drag's live preview), as the panel
    // holds it. The hit regions are rebuilt by Draw, so on a loaded VM the
    // first click can land before the window has drawn: try again a few times.
    // (The test is about the retarget, not about window-draw timing; the
    // mutation check below breaks the retarget, not the drag.)
    float heard = 0.0f;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0) snooze(500000);
        postMouse(B_MOUSE_DOWN, from, fromScreen);
        snooze(120000);
        postMouse(B_MOUSE_MOVED, to, toScreen);
        snooze(250000);
        if (fw->LockWithTimeout(1000000) == B_OK) {
            EffectsView* v = dynamic_cast<EffectsView*>(view);
            if (v != nullptr && !v->Chain().empty()
                && !v->Chain()[0].params.empty())
                heard = v->Chain()[0].params[0];
            fw->Unlock();
        }
        if (heard != LimiterDesc().p(0)) break;   // the drag reached the panel
    }
    std::printf("  drag: panel value %.2f (default %.2f)\n", heard,
                LimiterDesc().p(0));
    CHECK(heard != LimiterDesc().p(0));

    // Point the window at ANOTHER track's chain, mid-drag: the edit in flight
    // must be settled onto the chain it is leaving.
    BMessage show2(kMsgShowFx);
    show2.AddInt64("track", (int64)b);
    show2.AddInt32("focus", 0);
    win->PostMessage(&show2);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));  // still one
    snooze(400000);

    // The value the engine played is now the MODEL's -- and the window edits
    // the other chain (one window, retargeted: its title names the new track).
    float modelled = -1000.0f;
    win->Lock();
    if (Track* mt = project.FindTrack(a))
        if (!mt->fx.empty()) modelled = mt->fx[0].p(0);
    win->Unlock();
    CHECK(modelled == heard);
    CHECK(std::strstr(WindowTitle(fw), bName.c_str()) != nullptr);

    // Close it, back to the window count the rest of the run expects.
    fw->Lock();
    fw->Quit();
    snooze(200000);
    CHECK(WaitFor([&] { return VisibleWindows() == before; }));
}

// --- the theme modes (T1) --------------------------------------------------

// What the mode switch has to do that a unit test cannot see: leave the
// system's own colours alone, install the mode's control look, and repaint the
// windows that cached a colour.
// M3.6: the Project Settings window. It opens from the File menu, it is single
// instance, it shows the PROJECT's own values, and Apply turns what changed
// into ONE undoable edit (one "Project Settings" step in the Edit menu, not
// four). The model arithmetic behind it is host-tested; what this drives is the
// wiring: window -> message -> CommandStack -> model -> window.
static void TestProjectSettings(MainWindow* win, Project& project,
                                CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("project settings ...\n");

    win->PostMessage(MSG_PROJECT_SETTINGS);
    CHECK(WaitFor([&] { return FindVisibleWindow("Project Settings") != nullptr; }));
    Shot("project-settings");

    // The settings vocabulary (the UX-shell pass): Cancel + Apply, Apply is the
    // default, Escape closes. The old pair was "Close" (which discarded) next
    // to a Preferences window whose "Close" meant "done" -- one word, two
    // meanings, and neither said which.
    CheckVocabulary(FindVisibleWindow("Project Settings"), "Project Settings",
                    "ap", "Apply");

    // One window: asking again activates it.
    win->PostMessage(MSG_PROJECT_SETTINGS);
    snooze(400000);
    CHECK(CountVisibleWindows("Project Settings") == 1);

    // What the window shows is what the project IS (not its own defaults).
    double beforeRate = 0.0, beforeTempo = 0.0;
    std::string beforeNotes;
    int beforeNum = 0, beforeDenom = 0;
    {
        // The project first, on the main window's lock -- and let it go before
        // the dialog's lock is taken: locking one window inside the other's
        // lock is how a suite wedges.
        win->Lock();
        beforeRate  = project.sampleRate;
        beforeTempo = project.tempoBPM;
        beforeNotes = project.notes;
        beforeNum   = project.tempoMap.Meters().front().num;
        beforeDenom = project.tempoMap.Meters().front().denom;
        win->Unlock();
    }
    {
        BWindow* ps = FindVisibleWindow("Project Settings");
        if (ps != nullptr && ps->LockWithTimeout(1000000) == B_OK) {
            const ProjectSettingsView shown =
                static_cast<ProjectSettingsWindow*>(ps)->Values();
            CHECK(std::fabs(shown.sampleRate - beforeRate) < 0.5);
            CHECK(std::fabs(shown.tempoBPM - beforeTempo) < 1e-6);
            CHECK(shown.meterNum == beforeNum && shown.meterDenom == beforeDenom);
            CHECK(shown.notes == beforeNotes);

            // Change the tempo, the meter and the notes, and apply.
            static_cast<ProjectSettingsWindow*>(ps)->TempoField()
                ->SetText("128");
            BMenu* mm = static_cast<ProjectSettingsWindow*>(ps)->MeterField()->Menu();
            for (int32 i = 0; i < mm->CountItems(); i++)
                if (mm->ItemAt(i) != nullptr)
                    mm->ItemAt(i)->SetMarked(
                        std::strcmp(mm->ItemAt(i)->Label(), "6/8") == 0);
            static_cast<ProjectSettingsWindow*>(ps)->NotesView()
                ->SetText("take 3 is the keeper");
            static_cast<ProjectSettingsWindow*>(ps)->PostChanges();
            ps->Unlock();
        }
    }
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = std::fabs(project.tempoBPM - 128.0) < 1e-6
                     && project.tempoMap.Meters().front().num == 6
                     && project.tempoMap.Meters().front().denom == 8
                     && project.notes == "take 3 is the keeper";
        win->Unlock();
        return ok;
    }));
    // The project's own timeSig followed the map (they are one fact).
    win->Lock();
    CHECK(project.timeSig.numerator == 6 && project.timeSig.denominator == 8);
    // ... and the three changes are ONE edit: the Edit menu's next undo is the
    // whole thing (the macro), not one of its parts.
    CHECK(stack.UndoName() == "Project Settings");
    win->Unlock();
    Shot("project-settings-applied");

    // The window was told what was applied (a stale dialog is a bug).
    {
        BWindow* ps = FindVisibleWindow("Project Settings");
        if (ps != nullptr && ps->LockWithTimeout(1000000) == B_OK) {
            const ProjectSettingsView shown =
                static_cast<ProjectSettingsWindow*>(ps)->Values();
            CHECK(std::fabs(shown.tempoBPM - 128.0) < 1e-6);
            CHECK(shown.notes == "take 3 is the keeper");
            ps->Unlock();
        }
    }

    // One undo puts all three back: the macro is one step.
    win->PostMessage(MSG_UNDO_TEST);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool back = std::fabs(project.tempoBPM - beforeTempo) < 1e-6
                       && project.notes == beforeNotes
                       && project.tempoMap.Meters().front().num == beforeNum;
        win->Unlock();
        return back;
    }));

    // The sample rate: the same path, and the map's rate follows the project's
    // (the whole-project rescale is host-tested; this is the wiring).
    {
        BWindow* ps = FindVisibleWindow("Project Settings");
        if (ps != nullptr && ps->LockWithTimeout(1000000) == B_OK) {
            ProjectSettingsWindow* pw = static_cast<ProjectSettingsWindow*>(ps);
            BMenu* rm = pw->RateField()->Menu();
            for (int32 i = 0; i < rm->CountItems(); i++)
                if (rm->ItemAt(i) != nullptr
                    && std::strstr(rm->ItemAt(i)->Label(), "96000") != nullptr)
                    rm->ItemAt(i)->SetMarked(true);
            pw->ResampleCheck()->SetValue(B_CONTROL_OFF);   // no file work here
            pw->PostChanges();
            ps->Unlock();
        }
    }
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = std::fabs(project.sampleRate - 96000.0) < 1e-9
                     && std::fabs(project.tempoMap.sampleRate - 96000.0) < 1e-9;
        win->Unlock();
        return ok;
    }));
    // ... and its undo puts the project back exactly (a Project copy).
    win->PostMessage(MSG_UNDO_TEST);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool back = std::fabs(project.sampleRate - beforeRate) < 1e-9;
        win->Unlock();
        return back;
    }));

    HideOtherWindows(win);
    CHECK(WaitQuiet());
}

// M3.7: File > New offers the templates. The suite runs from a build tree, so
// the app finds data/templates beside its own binary; the directory is named
// explicitly here so the check does not depend on that copy having happened.
static void TestNewProjectTemplates(MainWindow* win, Project& project,
                                    CommandStack& stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("new project templates ...\n");

    // The same lookup the app makes (the suite binary sits beside the app):
    // point the environment at it so this test drives the code path a build
    // tree uses, not the installed one.
    char imageBuf[B_PATH_NAME_LENGTH];
    std::string buildTemplates;
    if (find_path(B_APP_IMAGE_SYMBOL, B_FIND_PATH_IMAGE_PATH, NULL, imageBuf,
                  sizeof(imageBuf)) == B_OK) {
        std::string image(imageBuf);
        const size_t slash = image.find_last_of('/');
        if (slash != std::string::npos)
            buildTemplates = image.substr(0, slash) + "/data/templates";
    }
    if (!buildTemplates.empty())
        setenv("DAW_TEMPLATES_DIR", buildTemplates.c_str(), 1);

    win->PostMessage(MSG_NEW_PROJECT);
    CHECK(WaitFor([&] { return FindVisibleWindow("New Project") != nullptr; }));
    Shot("new-project");

    // An action dialog: Cancel + the verb, Return presses Create, Escape closes.
    CheckVocabulary(FindVisibleWindow("New Project"), "New Project", "ok",
                    "Create");

    // The rows the shipped templates produce (when the copy rule ran, which is
    // what makes a build tree usable at all).
    {
        BWindow* np = FindVisibleWindow("New Project");
        if (np != nullptr && np->LockWithTimeout(1000000) == B_OK) {
            const std::vector<TemplateInfo>& rows =
                static_cast<NewProjectWindow*>(np)->Templates();
            bool hasEmpty = false, hasBand = false, hasProducer = false;
            for (const TemplateInfo& t : rows) {
                if (t.id == "empty")    hasEmpty = true;
                if (t.id == "band")     hasBand = true;
                if (t.id == "producer") hasProducer = true;
            }
            CHECK(hasEmpty);
            CHECK(!rows.empty() && rows[0].id == "empty");
            if (!buildTemplates.empty()) {
                CHECK(hasBand);
                CHECK(hasProducer);
            }
            // Asking again brings the open dialog forward instead of stacking.
            np->Unlock();
        }
    }
    win->PostMessage(MSG_NEW_PROJECT);
    snooze(400000);
    CHECK(CountVisibleWindows("New Project") == 1);

    // Choose Band and create it: the session is replaced by the template's.
    {
        BWindow* np = FindVisibleWindow("New Project");
        if (np != nullptr && np->LockWithTimeout(1000000) == B_OK) {
            NewProjectWindow* chooser = static_cast<NewProjectWindow*>(np);
            chooser->SelectId("band");
            CHECK(chooser->SelectedId() == "band");
            chooser->PostChosen();
            np->Unlock();
        }
    }
    // The demo session is dirty by now, so the prompt comes up first.
    if (WaitFor([&] { return AlertUp("Unsaved Changes"); }, 3000000))
        AnswerAlertWhenUp("Unsaved Changes", 1);       // Discard
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool ok = project.Tracks().size() == 9 && !stack.IsDirty();
        win->Unlock();
        return ok;
    }));
    // The template's shape arrived: 8 audio tracks into a bus, named, at the
    // default sample rate (the template's tracks, not its rate, is what it
    // brings).
    {
        win->Lock();
        int audio = 0, busy = 0;
        bool named = true;
        for (const Track& t : project.Tracks()) {
            if (t.type == TrackType::Audio) audio++;
            if (t.type == TrackType::Bus)   busy++;
            if (t.name.empty()) named = false;
        }
        CHECK(audio == 8);
        CHECK(busy == 1);
        CHECK(named);
        win->Unlock();
    }
    Shot("new-project-band");

    // A missing template directory is a normal state, not a crash or a dead
    // end: the dialog still opens, offering the built-in Empty.
    setenv("DAW_TEMPLATES_DIR", "/tmp/haiku_daw_no_such_templates", 1);
    win->PostMessage(MSG_NEW_PROJECT);
    CHECK(WaitFor([&] { return FindVisibleWindow("New Project") != nullptr; }));
    {
        BWindow* np = FindVisibleWindow("New Project");
        if (np != nullptr && np->LockWithTimeout(1000000) == B_OK) {
            NewProjectWindow* chooser = static_cast<NewProjectWindow*>(np);
            // The env override points at nothing, but the build-tree copy is
            // still a candidate, so at least Empty must be there and the
            // dialog must be answerable.
            CHECK(!chooser->Templates().empty());
            CHECK(chooser->Templates()[0].id == "empty");
            chooser->SelectId("empty");
            chooser->PostChosen();       // and it answers: Create
            np->Unlock();
        }
    }
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        const bool empty = project.Tracks().empty();
        win->Unlock();
        return empty;
    }));
    unsetenv("DAW_TEMPLATES_DIR");

    HideOtherWindows(win);
    CHECK(WaitQuiet());
}

// M3.5: the Preferences window. It opens from the File menu item, it is single
// instance, it SHOWS the app's own values, and a change made in it reaches the
// app, is pushed back to the window, and survives a reload of the settings
// file. (The Audio menu's radio items and View's Dark Mode live here now; the
// window is driven the way a user drives it, minus the menus that would block
// in BPopUpMenu::Go.)
static void TestPreferences(MainWindow* win, Project& project) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("preferences window ...\n");

    auto findPrefs = [&]() -> BWindow* {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strcmp(WindowTitle(w), "Preferences") == 0)
                return w;
        }
        return nullptr;
    };
    auto countPrefs = [&] {
        int n = 0;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strcmp(WindowTitle(w), "Preferences") == 0)
                n++;
        }
        return n;
    };
    // What the window is showing, read under its lock.
    auto shownValues = [&]() {
        PrefsView v;
        BWindow* w = findPrefs();
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            v = static_cast<PreferencesWindow*>(w)->Values();
            w->Unlock();
        }
        return v;
    };
    // Drive one control the way a click does: set it, then take the window's
    // own change path (both under the lock -- a view method runs on its
    // window's thread).
    auto edit = [&](const std::function<void(PreferencesWindow*)>& fn) {
        BWindow* w = findPrefs();
        if (w == nullptr || w->LockWithTimeout(1000000) != B_OK) return false;
        fn(static_cast<PreferencesWindow*>(w));
        static_cast<PreferencesWindow*>(w)->NotifyChanged();
        w->Unlock();
        return true;
    };

    // What the app had before the window is opened, so the test can put it
    // back and leave the rest of the run (and the settings file) as it found
    // them. The scale is kept BOTH ways: the live theme scale (what the screen
    // is drawn at) and the stored preference the window shows (0 = "follow the
    // system font", which is a value and not a scale).
    const int beforeBuffer = (int)win->BufferFramesForTest();
    const int beforeCountIn = (int)win->CountInBarsForTest();
    const ThemeMode beforeMode = ActiveThemeMode();
    const float beforeScaleLive = ThemeScale();
    const float beforeScalePref = shownValues().uiScale;

    // Mark the menu item whose label carries this number ("1024 frames",
    // "125%", "4 bars", "Off" = 0). Restoring by VALUE, not by index, is what
    // makes the test indifferent to the settings file it inherits: a run that
    // crashed mid-way leaves one behind, and a hard-coded restore then makes a
    // different change than the one it is undoing.
    auto markValue = [](BMenu* m, int value) {
        if (m == nullptr) return false;
        for (int32 i = 0; i < m->CountItems(); i++) {
            BMenuItem* it = m->ItemAt(i);
            if (it == nullptr || it->Label() == nullptr) continue;
            if (std::atoi(it->Label()) == value) {
                it->SetMarked(true);
                return true;
            }
        }
        return false;
    };

    win->PostMessage(MSG_PREFERENCES);
    CHECK(WaitFor([&] { return findPrefs() != nullptr; }));
    Shot("preferences");

    // The vocabulary (the UX-shell pass): a SETTINGS window is Cancel + Apply,
    // its Apply is the default button (Return applies), and Escape closes it
    // (the same B_QUIT_REQUESTED Cancel posts -- so Escape discards what was
    // not applied, exactly like Cancel).
    {
        BWindow* w = findPrefs();
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            DawButton* cancel = dynamic_cast<DawButton*>(w->FindView("cx"));
            DawButton* apply  = dynamic_cast<DawButton*>(w->FindView("ap"));
            CHECK(cancel != nullptr && cancel->Label() != nullptr
                  && std::strcmp(cancel->Label(), "Cancel") == 0);
            CHECK(apply != nullptr && apply->Label() != nullptr
                  && std::strcmp(apply->Label(), "Apply") == 0);
            CHECK(apply != nullptr && w->DefaultButton() == apply);
            CHECK((w->Flags() & B_CLOSE_ON_ESCAPE) != 0);
            // The device row says what is true for a user -- the system's
            // default is used -- and not what a roadmap note said.
            if (BMenuField* dv = dynamic_cast<BMenuField*>(w->FindView("dv")))
                CHECK(dv->Menu() != nullptr && dv->Menu()->ItemAt(0) != nullptr
                      && std::strcmp(dv->Menu()->ItemAt(0)->Label(),
                                     "System default") == 0);
            if (BStringView* hint = dynamic_cast<BStringView*>(w->FindView("dvh")))
                CHECK(BString(hint->Text()).FindFirst("not available yet") >= 0);
            w->Unlock();
        }
    }

    // It shows the app's values, not its own defaults.
    {
        const PrefsView v = shownValues();
        CHECK(v.bufferFrames == beforeBuffer);
        CHECK(v.countInBars == beforeCountIn);
        CHECK(v.darkMode == (beforeMode == ThemeMode::Dark));
    }
    // ... and it SAYS what its two timing-sensitive rows promise (M3.5): the
    // buffer at the next Play, the scale now. A row whose effect is not where
    // the user looks for it is a row that appears not to work.
    {
        BWindow* w = findPrefs();
        bool haveBufferHint = false, haveScaleHint = false;
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            if (BStringView* h = dynamic_cast<BStringView*>(w->FindView("bfh")))
                haveBufferHint = BString(h->Text()).FindFirst("next Play") >= 0;
            if (BStringView* h = dynamic_cast<BStringView*>(w->FindView("ush")))
                // "Applied on Apply" since the vocabulary pass gave the window
                // a Cancel/Apply pair: the row is a setting until then.
                haveScaleHint = BString(h->Text()).FindFirst("Applied on Apply")
                                >= 0;
            w->Unlock();
        }
        CHECK(haveBufferHint);
        CHECK(haveScaleHint);
    }

    // One window: asking again activates this one (the mixer's rule).
    win->PostMessage(MSG_PREFERENCES);
    snooze(400000);
    CHECK(countPrefs() == 1);

    // A SETTING is not a commit (the UX-shell pass): changing a control in the
    // window changes nothing outside it until Apply -- this used to apply on
    // every pick, so there was no way to look at a choice before taking it.
    // (A menu row is marked here the way the existing edits do it, but WITHOUT
    // the window's change path: marking is what a user's click does.)
    const int uncommitted = beforeCountIn == 4 ? 2 : 4;
    {
        BWindow* w = findPrefs();
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            BMenu* m = static_cast<PreferencesWindow*>(w)->CountInField()->Menu();
            for (int32 i = 0; i < m->CountItems(); i++)
                if (BMenuItem* it = m->ItemAt(i))
                    it->SetMarked(it->Label() != nullptr
                                  && std::atoi(it->Label()) == uncommitted);
            w->Unlock();
        }
    }
    snooze(300000);
    CHECK((int)win->CountInBarsForTest() == beforeCountIn);   // the app is untouched
    CHECK(shownValues().countInBars == uncommitted);          // the window shows it

    // Apply is a real button of the window: invoking it is what a click does,
    // and it commits the whole view.
    {
        BWindow* w = findPrefs();
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            if (DawButton* apply = dynamic_cast<DawButton*>(w->FindView("ap")))
                apply->Invoke();
            w->Unlock();
        }
    }
    CHECK(WaitFor([&] { return (int)win->CountInBarsForTest() == uncommitted; }));

    // Cancel -- and Escape, which posts the same B_QUIT_REQUESTED -- leaves
    // WITHOUT applying what the window was holding. Checked on the dark-mode
    // box: flipping it and cancelling must not switch the look.
    {
        const ThemeMode mode = ActiveThemeMode();
        BWindow* w = findPrefs();
        if (w != nullptr && w->LockWithTimeout(1000000) == B_OK) {
            if (BCheckBox* dark = dynamic_cast<BCheckBox*>(w->FindView("dm")))
                dark->SetValue(mode == ThemeMode::Dark ? B_CONTROL_OFF
                                                       : B_CONTROL_ON);
            w->Unlock();
        }
        if (w != nullptr) w->PostMessage(B_QUIT_REQUESTED);   // what Escape posts
        CHECK(WaitFor([&] { return findPrefs() == nullptr; }));
        snooze(300000);
        CHECK(ActiveThemeMode() == mode);
    }
    win->PostMessage(MSG_PREFERENCES);   // back for the rest of the test
    CHECK(WaitFor([&] { return findPrefs() != nullptr; }));
    snooze(200000);

    // Count-in: pick "4 bars" in the window's menu and take its change path.
    CHECK(edit([](PreferencesWindow* w) {
        BMenu* m = w->CountInField()->Menu();
        for (int32 i = 0; i < m->CountItems(); i++)
            m->ItemAt(i)->SetMarked(i == 3);      // kCountInChoices[3] = 4
    }));
    CHECK(WaitFor([&] { return win->CountInBarsForTest() == 4; }));

    // Buffer size: what the row PROMISES is "applied at the next Play" — the
    // device is not re-opened under a running transport — so BOTH halves are
    // checked: the value the app now holds, and the engine that the next Play
    // builds with it. (The check the row makes is honest only if the Play
    // really uses it; a value nothing reads would pass a state check and still
    // be a dead control.)
    CHECK(edit([](PreferencesWindow* w) {
        BMenu* m = w->BufferField()->Menu();
        for (int32 i = 0; i < m->CountItems(); i++)
            m->ItemAt(i)->SetMarked(i == 3);      // kBufferFrameChoices[3] = 1024
    }));
    CHECK(WaitFor([&] { return win->BufferFramesForTest() == 1024; }));
    win->PostMessage(MSG_PLAY);
    CHECK(WaitFor([&] { return win->EngineBufferFramesForTest() == 1024; }));
    win->PostMessage(MSG_STOP);
    CHECK(WaitFor([&] { return !win->IsPlaying(); }));

    // The UI scale: applied NOW, process-wide (Theme.h), and the app's own
    // record of it moves with it — a record that disagreed with the screen is
    // how a 125% row ended up over a 100% layout.
    CHECK(edit([](PreferencesWindow* w) {
        BMenu* m = w->ScaleField()->Menu();
        for (int32 i = 0; i < m->CountItems(); i++)
            m->ItemAt(i)->SetMarked(i == 2);      // kUiScaleChoices[2] = 1.25
    }));
    CHECK(WaitFor([&] { return std::fabs(ThemeScale() - 1.25f) < 1e-3f; }));
    CHECK(shownValues().uiScale == 1.25f);

    // Dark mode: the control that moved out of the View menu still flips the
    // whole look (TestThemeMode covers the mode switch itself).
    CHECK(edit([&](PreferencesWindow* w) {
        w->DarkCheck()->SetValue(ActiveThemeMode() == ThemeMode::Dark
                                 ? B_CONTROL_OFF : B_CONTROL_ON);
    }));
    CHECK(WaitFor([&] { return ActiveThemeMode() != beforeMode; }));

    // Everything the window did is in the settings file: reload it the way a
    // restart would and read the values back out of the window. The SCALE is
    // the interesting one here — a reload reads the preference and must apply
    // it, or the app remembers 125% while drawing at 100%.
    win->ReloadSettings();
    win->PostMessage(MSG_PREFERENCES);
    snooze(400000);
    {
        const PrefsView v = shownValues();
        CHECK(v.countInBars == 4);        // the choices the window made came
        CHECK(v.bufferFrames == 1024);    // back out of the settings file
        CHECK(v.uiScale == 1.25f);
    }
    CHECK(WaitFor([&] { return std::fabs(ThemeScale() - 1.25f) < 1e-3f; }));

    // Put the run's own state back, by value: the settings file is one of the
    // files the next test run inherits, and a run that crashed before this
    // point leaves the values it had reached.
    CHECK(edit([&](PreferencesWindow* w) {
        return markValue(w->BufferField()->Menu(), beforeBuffer);
    }));
    CHECK(WaitFor([&] { return win->BufferFramesForTest() == beforeBuffer; }));
    CHECK(edit([&](PreferencesWindow* w) {
        return markValue(w->CountInField()->Menu(), beforeCountIn);
    }));
    CHECK(WaitFor([&] { return win->CountInBarsForTest() == beforeCountIn; }));
    CHECK(edit([&](PreferencesWindow* w) {
        return markValue(w->ScaleField()->Menu(),
                         (int)(beforeScalePref * 100.0f + 0.5f));
    }));
    CHECK(WaitFor([&] {
        return std::fabs(ThemeScale()
                         - (beforeScalePref > 0.0f ? beforeScalePref
                                                   : beforeScaleLive)) < 1e-3f;
    }));
    if (ActiveThemeMode() != beforeMode) {
        win->PostMessage(MSG_THEME_MODE);
        CHECK(WaitFor([&] { return ActiveThemeMode() == beforeMode; }));
    }

    HideOtherWindows(win);   // the window is ours; put it away
    CHECK(WaitQuiet());
}

// The mixer and the windows it shares a session with (package 52). Every check
// here drives the REAL widget: the messages the strips post, the file-panel
// ref an Open arrives as, and a key sent to the mixer window the way the input
// server delivers one.
//
// It runs LAST among the project-touching tests because the last section
// replaces the session (an Open), which is the point of it.
static void TestUxMixer(MainWindow* win, Project& project, CommandStack* stack) {
    HideOtherWindows(win);
    CHECK(WaitQuiet());
    std::printf("test_ux_mixer\n");

    // --- fixture: two named tracks, so a strip list has something to show ---
    Track a;
    a.id = project.NextTrackId();
    a.type = TrackType::Audio;
    a.name = "ux-a";
    CHECK(LockedAddTrack(win, project, a));
    Track b;
    b.id = project.NextTrackId();
    b.type = TrackType::Audio;
    b.name = "ux-b";
    CHECK(LockedAddTrack(win, project, b));

    // --- the rack: one strip per track, and it FOLLOWS a track added --------
    win->PostMessage(MSG_MIXER_TEST);
    MixerStripsView* strips = nullptr;
    BWindow* mixer = nullptr;
    CHECK(WaitFor([&] {
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (!w || w->IsHidden() || w == win) continue;
            if (w->LockWithTimeout(500000) != B_OK) continue;
            BView* v = w->FindView("strips");
            if (v) { mixer = w; strips = dynamic_cast<MixerStripsView*>(v); }
            w->Unlock();
            if (strips) return true;
        }
        return false;
    }));
    CHECK(strips != nullptr);
    if (strips == nullptr) return;

    auto stripCount = [&] {
        size_t n = 0;
        if (mixer->LockWithTimeout(1000000) == B_OK) {
            n = strips->Strips().size();
            mixer->Unlock();
        }
        return n;
    };
    auto stripNames = [&] {
        std::string s;
        if (mixer->LockWithTimeout(1000000) == B_OK) {
            for (const MixerStripInfo& st : strips->Strips()) s += st.name + ",";
            mixer->Unlock();
        }
        return s;
    };
    const size_t before = stripCount();
    CHECK(before >= 2);
    CHECK(stripNames().find("ux-a") != std::string::npos);
    std::printf("  rack: %zu strips [%s]\n", before, stripNames().c_str());

    // Track > New Audio goes through the app's own menu handler, and the rack
    // has to grow the strip for it: it used to need an unrelated edit (a mute
    // toggle, a marker rename) to show up at all.
    win->PostMessage(MSG_NEW_AUDIO);
    CHECK(WaitFor([&] { return stripCount() > before; }));
    const size_t afterAdd = stripCount();
    CHECK(WaitFor([&] { return stripNames().find("Audio") != std::string::npos; }));
    std::printf("  after Track > New Audio: %zu strips [%s]\n", afterAdd,
                stripNames().c_str());

    // A track deletion (the model + the refresh a delete posts) takes its strip
    // with it. Left up, that strip looked live and dropped every edit silently.
    {
        TrackId gone = kInvalidTrackId;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (!project.Tracks().empty()) gone = project.Tracks().back().id;
            win->Unlock();
        }
        if (gone != kInvalidTrackId) {
            if (win->LockWithTimeout(1000000) == B_OK) {
                stack->Execute(std::make_unique<RemoveTrackCommand>(gone), project);
                win->Unlock();
            }
            win->PostMessage(kMsgUiRefresh);
            CHECK(WaitFor([&] { return stripCount() == afterAdd - 1; }));
            std::printf("  after deleting a track: %zu strips\n", stripCount());
        }
    }

    // The rack as it stands: the shot is taken with the mixer in front, so what
    // is reviewed is the console -- strips, meters, lamps and the master.
    if (mixer->LockWithTimeout(1000000) == B_OK) {
        mixer->Activate(true);
        mixer->Unlock();
    }
    snooze(300000);
    Shot("ux-mixer");

    // --- the master fader is an undoable, DIRTYING edit (item D) ------------
    // It used to write project.masterGain directly, so there was no Cmd-Z for a
    // master move and a session whose only change was the level reported clean:
    // File > Close gave no prompt and the level was lost.
    {
        float was = 1.0f;
        bool wasDirty = false;
        if (win->LockWithTimeout(1000000) == B_OK) {
            was = project.masterGain;
            wasDirty = stack->IsDirty();
            win->Unlock();
        }
        BMessage m(kMsgApplyMaster);
        m.AddFloat("gain", 0.5f);
        win->PostMessage(&m);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = std::fabs(project.masterGain - 0.5f) < 1e-3f;
            win->Unlock();
            return ok;
        }));
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool dirtied = stack->IsDirty();
            const bool named = stack->UndoName() == "Set Master Gain";
            win->Unlock();
            return dirtied && named;
        }));
        std::printf("  master fader: an undoable, dirtying edit\n");
        // And it undoes, which the direct write never could.
        win->PostMessage(MSG_EDIT_UNDO);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool ok = std::fabs(project.masterGain - was) < 1e-3f;
            win->Unlock();
            return ok;
        }));
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool same = stack->IsDirty() == wasDirty;
            win->Unlock();
            return same;
        }));
    }

    // --- the keyboard drives the rack (item P) -----------------------------
    // There was no focus and no KeyDown at all: a mixer could only be mixed
    // with a mouse. A click on the strip's NAME bar (never a control that opens
    // a popup -- that would wedge the run) makes it current and focuses the view.
    //
    // THE KEYS ARE POSTED TO THE LOOPER'S PREFERRED HANDLER (a null BHandler),
    // not to the window. That is not a detail: Haiku only routes a key to the
    // FOCUS VIEW when the message is a "focus message", i.e. its header target
    // is B_PREFERRED_TOKEN (BWindow::task_looper -> _DetermineTarget ->
    // CurrentFocus, and BWindow::_IsFocusMessage, which bails out otherwise).
    // A key posted with the WINDOW's own token -- window->PostMessage, or
    // BMessenger(window) -- is not one, so BWindow::MessageReceived simply
    // drops it. Real events from the app_server carry the preferred token,
    // which is what makes this the faithful way to drive them.
    {
        BPoint click(50.0f, 0.0f);        // on the strip's NAME bar
        BPoint screen = click;
        if (mixer->LockWithTimeout(1000000) == B_OK) {
            click.y = strips->Bounds().Height() - 12.0f;
            screen = strips->ConvertToScreen(click);
            mixer->Unlock();
        }
        BMessage md(B_MOUSE_DOWN);
        md.AddInt32("buttons", 1);
        md.AddInt32("clicks", 1);
        md.AddPoint("where", click);
        md.AddPoint("screen_where", screen);
        BMessenger(strips).SendMessage(&md);
        BMessage mu(B_MOUSE_UP);
        mu.AddInt32("buttons", 0);
        mu.AddInt32("clicks", 1);
        mu.AddPoint("where", click);
        mu.AddPoint("screen_where", screen);
        BMessenger(strips).SendMessage(&mu);
        snooze(100000);

        // Where the keys will land, and whether the rack took the focus at all.
        // Printed on EVERY run: "the keys do nothing" and "the view never took
        // focus" are different bugs, and this is the line that tells them apart.
        BHandler* focus = nullptr;
        int cur = -1;
        if (mixer->LockWithTimeout(1000000) == B_OK) {
            cur = strips->CurrentStrip();
            focus = mixer->CurrentFocus();
            mixer->Unlock();
        }
        const char* focusName = "(none)";
        if (focus != nullptr && focus->Name() != nullptr)
            focusName = focus->Name();
        std::printf("  mixer keyboard: current strip %d of %zu, focus '%s'\n",
                    cur, stripCount(), focusName);
        CHECK(cur == 0);
        CHECK(focus == strips);   // the view took the focus (B_NAVIGABLE + MakeFocus)

        BMessenger toFocus((const BHandler*)nullptr, (const BLooper*)mixer);
        CHECK(toFocus.IsValid());

        // Down-arrow nudges the strip's gain by 0.5 dB, through the same
        // message path a mouse drag takes (so it is one undoable edit).
        float wasGain = 0.0f;
        TrackId firstId = kInvalidTrackId;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (!project.Tracks().empty()) {
                wasGain = project.Tracks().front().gain;
                firstId = project.Tracks().front().id;
            }
            win->Unlock();
        }
        BMessage key(B_KEY_DOWN);
        const char down = (char)B_DOWN_ARROW;
        key.AddString("bytes", std::string(1, down).c_str());
        key.AddInt32("modifiers", 0);
        toFocus.SendMessage(&key);
        const bool nudged = WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* t = project.FindTrack(firstId);
            const bool ok = t && t->gain < wasGain - 1e-3f;
            win->Unlock();
            return ok;
        });
        float nowGain = wasGain;
        if (win->LockWithTimeout(1000000) == B_OK) {
            if (const Track* t = project.FindTrack(firstId)) nowGain = t->gain;
            win->Unlock();
        }
        // Printed either way: a key that changed nothing is a fact the next
        // reader needs, and the success printf alone hides it.
        std::printf("  mixer keyboard: gain %.4f -> %.4f (%s)\n", wasGain,
                    nowGain, nudged ? "arrow" : "NO CHANGE");
        CHECK(nudged);
        // 'm' toggles the current strip's mute, through kMsgApplyMix.
        BMessage mk(B_KEY_DOWN);
        mk.AddString("bytes", "m");
        mk.AddInt32("modifiers", 0);
        toFocus.SendMessage(&mk);
        const bool muted = WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* t = project.FindTrack(firstId);
            const bool ok = t && t->muted;
            win->Unlock();
            return ok;
        });
        std::printf("  mixer keyboard: 'm' %s\n", muted ? "muted" : "NO CHANGE");
        CHECK(muted);
        // ... and put it back, so the fixture is as it was.
        toFocus.SendMessage(&mk);
        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const Track* t = project.FindTrack(firstId);
            const bool ok = t && !t->muted;
            win->Unlock();
            return ok;
        }));
    }

    // --- one sends window per editor, retargeted (item L) -------------------
    // Two windows on one track silently overwrote each other's edits, because
    // each commits its whole list.
    {
        const size_t w0 = (size_t)VisibleWindows();
        BMessage s1(kMsgMixSends);
        s1.AddInt64("track", (int64)a.id);
        win->PostMessage(&s1);
        CHECK(WaitFor([&] { return WindowUp("Sends \xE2\x80\x94 ux-a"); }));
        // The review's own note: this window and the instrument editor had no
        // Shot() anywhere, so nothing had ever LOOKED at them. Capture both.
        Shot("ux-sends");
        // An instrument editor for an AUDIO track: refused, because there is no
        // voice to edit there (and a window opened on a stale snapshot would
        // then commit a whole descriptor for the wrong kind of track).
        {
            const size_t w1 = (size_t)VisibleWindows();
            BMessage s2(kMsgMixInst);
            s2.AddInt64("track", (int64)a.id);
            win->PostMessage(&s2);
            snooze(300000);
            CHECK((size_t)VisibleWindows() == w1);
        }

        // Asking again for the same track: the SAME window is activated, not a
        // second copy of the send list.
        BMessage s3(kMsgMixSends);
        s3.AddInt64("track", (int64)a.id);
        win->PostMessage(&s3);
        snooze(200000);
        int sendsWindows = 0;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strncmp(WindowTitle(w), "Sends", 5) == 0)
                sendsWindows++;
        }
        CHECK(sendsWindows == 1);

        // Another track RETARGETS it: same window, its title now names ux-b.
        BMessage s4(kMsgMixSends);
        s4.AddInt64("track", (int64)b.id);
        win->PostMessage(&s4);
        CHECK(WaitFor([&] { return WindowUp("Sends \xE2\x80\x94 ux-b"); }));
        int sendsWindows2 = 0;
        for (int32 i = 0; i < be_app->CountWindows(); i++) {
            BWindow* w = be_app->WindowAt(i);
            if (w && !w->IsHidden()
                && std::strncmp(WindowTitle(w), "Sends", 5) == 0)
                sendsWindows2++;
        }
        CHECK(sendsWindows2 == 1);
        std::printf("  sends windows: same=%d retargeted=%d (%zu -> %zu windows)\n",
                    sendsWindows, sendsWindows2, w0, (size_t)VisibleWindows());

        // Deleting that track closes it (checked in the refresh funnel).
        if (win->LockWithTimeout(1000000) == B_OK) {
            stack->Execute(std::make_unique<RemoveTrackCommand>(b.id), project);
            win->Unlock();
        }
        win->PostMessage(kMsgUiRefresh);
        CHECK(WaitFor([&] { return !WindowUp("Sends \xE2\x80\x94 ux-b"); }));

        // The instrument editor, which the suite had only ever REFUSED (audio
        // track): open it for a MIDI track and look at it.
        Track m;
        m.id = project.NextTrackId();
        m.type = TrackType::Midi;
        m.name = "ux-midi";
        CHECK(LockedAddTrack(win, project, m));
        BMessage s6(kMsgMixInst);
        s6.AddInt64("track", (int64)m.id);
        win->PostMessage(&s6);
        CHECK(WaitFor([&] { return WindowUp("Instrument \xE2\x80\x94 ux-midi"); }));
        Shot("ux-instrument");
        {
            // ... and it is single-instance too, the same rule as the sends
            // editor above (one window, retargeted).
            BMessage again(kMsgMixInst);
            again.AddInt64("track", (int64)m.id);
            win->PostMessage(&again);
            snooze(200000);
            int instUp = 0;
            for (int32 i = 0; i < be_app->CountWindows(); i++) {
                BWindow* w = be_app->WindowAt(i);
                if (w && !w->IsHidden()
                    && std::strncmp(WindowTitle(w), "Instrument", 10) == 0)
                    instUp++;
            }
            CHECK(instUp == 1);
        }
        if (win->LockWithTimeout(1000000) == B_OK) {
            stack->Execute(std::make_unique<RemoveTrackCommand>(m.id), project);
            win->Unlock();
        }
        win->PostMessage(kMsgUiRefresh);
        CHECK(WaitFor([&] { return !WindowUp("Instrument \xE2\x80\x94 ux-midi"); }));
    }

    // --- A: an Open closes every window addressed by track id --------------
    // Track ids restart at 1 in a loaded file, so a panel left up edits the NEW
    // project's same-numbered track -- and the effects panel commits its whole
    // chain, which is a data-corrupting write saved as an undoable edit.
    {
        const char* openPath = "/tmp/haiku_daw_ui_uxmix.dawproj";
        std::remove(openPath);
        {
            Project other;
            other.sampleRate = project.sampleRate;
            CHECK(other.AddTrack(MakeMidiTrack(other, { { 60, 100, 0, 4800 } },
                                               "ux-opened")));
            CHECK(ProjectIO::Save(other, openPath));
        }
        // The editor windows that hold a track id, all up at once: the mixer,
        // the generic effects panel, the sends editor.
        win->PostMessage(MSG_MIXER_TEST);
        CHECK(WaitFor([&] { return WindowUp("Mixer"); }));
        {
            BMessage fx(kMsgShowFx);
            fx.AddInt64("track", (int64)a.id);
            fx.AddInt32("focus", -1);
            win->PostMessage(&fx);
            CHECK(WaitFor([&] {
                for (int32 i = 0; i < be_app->CountWindows(); i++) {
                    BWindow* w = be_app->WindowAt(i);
                    if (w && !w->IsHidden()
                        && std::strstr(WindowTitle(w), "ux-a") != nullptr)
                        return true;
                }
                return false;
            }));
        }
        BMessage s5(kMsgMixSends);
        s5.AddInt64("track", (int64)a.id);
        win->PostMessage(&s5);
        CHECK(WaitFor([&] { return WindowUp("Sends \xE2\x80\x94 ux-a"); }));

        int upBefore = VisibleWindows();
        std::printf("  before Open: %d window(s) up\n", upBefore);
        CHECK(upBefore >= 4);   // main + mixer + effects + sends

        // Dirty the project first, so the unsaved-changes prompt is the one
        // that appears (and Discard is the answer that loads).
        if (win->LockWithTimeout(1000000) == B_OK) {
            stack->Execute(std::make_unique<SetTrackNameCommand>(a.id, "ux-a2"),
                           project);
            win->Unlock();
        }
        entry_ref ref;
        CHECK(BEntry(openPath).GetRef(&ref) == B_OK);
        BMessage open(MSG_OPEN_REF);
        open.AddRef("refs", &ref);
        win->PostMessage(&open);
        CHECK(WaitFor([&] { return AlertUp("Unsaved Changes"); }));
        CHECK(AnswerAlertWhenUp("Unsaved Changes", 1));       // Discard

        CHECK(WaitFor([&] {
            if (win->LockWithTimeout(1000000) != B_OK) return false;
            const bool loaded = project.Tracks().size() == 1
                             && project.Tracks().front().name == "ux-opened";
            win->Unlock();
            return loaded;
        }));
        // Every one of them closed. This is the check the package exists for.
        CHECK(WaitFor([&] { return VisibleWindows() == 1; }, 20000000));
        std::printf("  after Open: %d window(s) up\n", VisibleWindows());
        CHECK(!WindowUp("Mixer"));
        CHECK(!WindowUp("Sends \xE2\x80\x94 ux-a"));
        std::remove(openPath);
    }

    HideOtherWindows(win);
    CHECK(WaitQuiet());
}

static void TestThemeMode(MainWindow* win) {    const ThemeMode runMode = ActiveThemeMode();   // DAW_UI_THEME for this pass

    // The user's own panel colour, before anything is switched. Haiku keeps it
    // in the app_server and SAVES it, which is why the app may never write it.
    const rgb_color systemPanel = ui_color(B_PANEL_BACKGROUND_COLOR);

    // The mode reached the window's own background (its top view is what shows
    // where no pane covers it).
    win->Lock();
    BLayout* layout = win->GetLayout();
    BView* top = layout != nullptr ? layout->Owner() : nullptr;
    // The timeline is a SIBLING of the menu bar and the transport strip, not
    // their child: reading it is what catches a refresh that only walks the
    // window's first child (which is exactly what shipped once -- every state
    // check passed while the screen never changed).
    BView* timeline = win->FindView("timeline");
    const rgb_color before = top != nullptr ? top->ViewColor()
                                            : B_TRANSPARENT_COLOR;
    const rgb_color laneBefore = timeline != nullptr
        ? timeline->ViewColor() : B_TRANSPARENT_COLOR;
    win->Unlock();
    CHECK(before == ColBackground());
    CHECK(laneBefore == ColBackground());

    win->PostMessage(MSG_THEME_MODE);   // View > Dark Mode
    CHECK(WaitFor([&] { return ActiveThemeMode() != runMode; }));

    win->Lock();
    const rgb_color after = top != nullptr ? top->ViewColor()
                                           : B_TRANSPARENT_COLOR;
    const rgb_color laneAfter = timeline != nullptr
        ? timeline->ViewColor() : B_TRANSPARENT_COLOR;
    win->Unlock();
    CHECK(after != before);            // the cached colour was re-taken
    CHECK(after == ColBackground());   // ... to the other mode's
    CHECK(laneAfter != laneBefore);    // and the panes were reached too
    CHECK(laneAfter == ColBackground());

    // The look followed the mode: the stock Haiku look in System mode,
    // DawControlLook in Dark mode.
    CHECK((dynamic_cast<DawControlLook*>(be_control_look) != nullptr)
          == (ActiveThemeMode() == ThemeMode::Dark));

    // And the operating system was left alone. This is the check that fails if
    // anything ever calls set_ui_color again.
    const rgb_color panelNow = ui_color(B_PANEL_BACKGROUND_COLOR);
    CHECK(panelNow.red == systemPanel.red
          && panelNow.green == systemPanel.green
          && panelNow.blue == systemPanel.blue);

    Shot(ActiveThemeMode() == ThemeMode::Dark ? "theme-dark" : "theme-system");

    // Back to the run's mode, so the shots of everything after this are in the
    // mode the pass was asked for.
    win->PostMessage(MSG_THEME_MODE);
    CHECK(WaitFor([&] { return ActiveThemeMode() == runMode; }));
    Shot(runMode == ThemeMode::Dark ? "theme-dark-2" : "theme-system-2");
}

// --- driver ----------------------------------------------------------------

#ifdef DAW_HAVE_LV2
// --- 8. LV2 insert state: the preset box on the insert panel ---------------
//
// What is left of the preset feature on this side of the glass: an LV2 insert's
// panel draws the Preset box, and the whole chain still round-trips through a
// panel commit with its state intact (the codec is what silently destroyed
// patches before). The menu itself blocks in BPopUpMenu::Go, so what a preset
// choice DOES is covered host-side (lv2_state_tests); what the panel LOOKS
// like is what this shot is for.
static void TestLv2InsertPresets(MainWindow* win, Project& project) {
    std::printf("test_lv2_insert_presets\n");
    Lv2Host::Instance().ScanAll();
    const std::vector<Lv2PluginInfo>& plugins = Lv2Host::Instance().Plugins();
    const Lv2PluginInfo* chosen = nullptr;
    for (const Lv2PluginInfo& p : plugins)
        if (!p.uri.empty()) { chosen = &p; break; }
    if (!chosen) {
        std::printf("  no hostable LV2 plugin installed - nothing to show\n");
        return;
    }

    Track t = MakeMidiTrack(project, {}, "lv2-preset");
    EffectDesc d = MakeInsertDesc(EffectType::Lv2, chosen->uri);
    d.state = "<urn:haiku-daw:test:state:1>\n    a pset:Preset ;\n"
              "    lv2:appliesTo <urn:haiku-daw:test:stateful> .\n";
    t.fx.push_back(d);
    const TrackId tid = t.id;
    CHECK(LockedAddTrack(win, project, t));

    // A panel commit -- what any knob move posts -- must not drop the state.
    BMessage apply(kMsgApplyFx);
    apply.AddInt64("track", (int64)tid);
    EncodeFxChain(apply, t.fx);
    win->PostMessage(&apply);
    CHECK(WaitFor([&] {
        if (win->LockWithTimeout(1000000) != B_OK) return false;
        Track* tr = project.FindTrack(tid);
        const bool ok = tr && !tr->fx.empty() && tr->fx[0].state == d.state;
        win->Unlock();
        return ok;
    }));

    BMessage show(kMsgShowFx);
    show.AddInt64("track", (int64)tid);
    show.AddInt32("focus", 0);
    const int before = VisibleWindows();
    win->PostMessage(&show);
    CHECK(WaitFor([&] { return VisibleWindows() == before + 1; }));
    snooze(300000);
    Shot("fx-window-lv2-preset");

    for (int32 i = 0; i < be_app->CountWindows(); i++) {
        BWindow* w = be_app->WindowAt(i);
        if (w == nullptr || w->IsHidden()) continue;
        const char* name = WindowTitle(w);
        if (name != nullptr && std::strstr(name, "lv2-preset") != nullptr) {
            if (w->Lock()) { w->Quit(); }
            break;
        }
    }
    CHECK(WaitFor([&] { return VisibleWindows() == before; }));
}
#endif

static int32 TestThread(void*) {
    // A leftover recovery file makes the window open its "Recover?" alert at
    // startup, and that alert BLOCKS the window thread until someone answers
    // it -- every check that needs the lock then times out, which reads
    // exactly like a hang (the same shape as M1.1's crash-dialog lesson).
    // The recovery prompt has its own coverage elsewhere; a test run starts
    // from a clean slate.
    {
        BPath settings;
        if (ProjectDocument::SettingsPath(settings) == B_OK) {
            BPath recovery(settings);
            if (recovery.Append("recovery.dawproj") == B_OK)
                BEntry(recovery.Path()).Remove();
            // ... and the SETTINGS file itself. It persists the panes (M1.4:
            // `uiinsp`, `uidockpage`), so a run that ended with the inspector
            // hidden makes every LATER run start with it hidden -- and
            // TestDockedEditor's two checks assume it starts visible, so the
            // toggle sequence comes out inverted and the run that follows
            // leaves it detached (which then breaks TestDockPages' plugin
            // pick, whose message targets the inspector). One run's pane state
            // must not decide the next run's result: same rule as the recovery
            // file above, and the same wedge the HANDOFF records.
            BPath cfg(settings);
            if (cfg.Append("settings") == B_OK)
                BEntry(cfg.Path()).Remove();
        }
    }

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
    Shot("startup");

    TestMessageRoundTrip(win, project);
    TestPianoRollQuantize(win, project, stack);
    TestPianoRollCcLane(win, project);
    TestPianoRollTransforms(win, project, stack);
    TestExportFlow(win, project);
    TestDeviceLatency();
    TestExportCancel(win, project);
    TestExportStems(win, project);
    TestExportLoopRange(win, project);
    // After the WAV-only flows above, so a non-WAV container left remembered
    // by these cannot change what they write.
    TestExportFormatDialog(win, project);
    TestExportFormats(win, project);
    TestAboutBox(win);
#ifdef DAW_HAVE_LV2
    TestLv2EditorWiring(win, project, stack);
    TestLv2InsertPresets(win, project);
#endif
    TestErrorReports(win, project, stack);
    TestKeyboardFocus(win, project);
    TestThemeScale(win, project);
    TestArrangeFeel(win, project);
    TestClipStretch(win, project, &stack);
    TestArrangeDepth(win, project, &stack);
    TestArrangeTracks(win, project, &stack);
    TestWidgetKit(win);
    TestDialogVocabulary(win, project);   // the UX-shell dialog vocabulary
    TestDockedEditor(win, project, &stack);
    TestRollModelSync(win, project, &stack);   // package 51: the roll vs the model
    TestShortcutsWindow(win);
    TestPreferences(win, project);   // M3.5: opens and puts itself away
    TestProjectSettings(win, project, stack);   // M3.6: applies and undoes
    TestKeyMap(win, project, stack);
    TestMetering(win, project, stack);
    TestTransportBar(win, project);
    TestRegionJob(win, project);
    TestRenderFeedback(win, project);   // the status line's job messages
    TestMixerStripExtras(win, project);
    TestDockPages(win, project, &stack);
    TestFxRetargetFlush(win, project);
    TestSidechainPicker(win, project);
    TestEngineGraphSwap(win, project, stack);
    TestPeakCache(win, project);
    TestSeamlessLoop(win, project);
    TestBigProjectPlayback(win, project);
    // Both replace the session, so they run here: New leaves no path behind,
    // so the unsaved-changes flow after it still exercises the save-panel
    // branch.
    TestNewProjectTemplates(win, project, stack);
    TestFileMenuFlows(win, project, stack);
    TestUnsavedChanges(win, project, stack);   // last: it replaces the project
    // The mixer and the windows it shares a session with (package 52). Also
    // replaces the project (its item-A check IS a project replacement), which
    // is why it comes after the unsaved-changes flow and before the theme
    // switch, which depends on no project state at all.
    TestUxMixer(win, project, &stack);
    // The theme switch flips the whole process and puts it back; last, so no
    // other test's shots depend on the mode it leaves behind.
    TestThemeMode(win);

    std::printf("\nui_functional_tests: %d checks, %d failures\n", g_checks,
                g_fails);
    std::fflush(stdout);
    win->LockWithTimeout(1000000);
    win->Quit();   // and with it the application
    return 0;
}

int main() {
    // Line-buffer stdout: with a pipe (ctest) the progress lines would sit in
    // the buffer until exit, and a hung run would show nothing at all.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    BApplication app("application/x-haiku-daw-uitests");
    if (app.InitCheck() != B_OK) {
        std::printf("ui_functional_tests: no app_server - skipping\n");
        return 77;   // CTest SKIP_RETURN_CODE
    }
    // The theme (T1): the run's mode comes from DAW_UI_THEME, so the whole
    // screenshot pass is run twice -- once per mode -- and what is reviewed is
    // what a user in that mode sees. Exactly what DawApplication::ReadyToRun
    // does: the user's colours in, the mode's look installed, no system
    // colour written.
    const char* themeEnv = std::getenv("DAW_UI_THEME");
    SetActiveThemeMode(ThemeModeFromString(themeEnv));
    ReadSystemBaseColors();
    // A third value, for the screenshot pass only: System mode on a LIGHT
    // Appearance (the colours Haiku ships with). The machine this runs on may
    // have any Appearance at all -- this machine's own is a dark one -- and the
    // light-panel case has to be reviewable without changing anyone's system
    // settings, which is what T1 is about.
    if (themeEnv != nullptr && std::strcmp(themeEnv, "light") == 0)
        SetSystemBaseColors(ThemeBase::Defaults());
    InstallControlLookForMode(ActiveThemeMode());
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
