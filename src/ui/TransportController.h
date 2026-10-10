// TransportController — the engine's lifetime and the transport's rolling
// state, moved out of MainWindow (M1.1, slice 2).
//
// Public members on purpose: this slice is a pure move, and the window still
// drives every one of them. The method moves (play, stop, rebuild, seek,
// loop) land on top of this in the same item, tightening the seams as they go;
// keeping the state in one place first is what makes those moves mechanical.
//
// M4.1: the engine is created on first use and KEPT — its BSoundPlayer survives
// every rebuild, and a rebuild is a graph built on the engine's worker thread
// and swapped in (Engine::RequestLoad), so nothing here blocks the window
// thread on a load and an edit during playback causes no gap.
#pragma once

#include "../engine/Engine.h"
#include "../model/Project.h"   // Project (ProjectEndFrame) + Frame

#include <memory>

namespace daw {

class MainWindow;

// The frame after the last clip/note (used by the play and record range
// planning). Declared here because the transport's moved bodies need it and
// the window's own code uses it too; defined in MainWindow.cpp.
Frame ProjectEndFrame(const Project& project);

class TransportController {
public:
    // The window this controller drives. Set once by MainWindow's constructor;
    // the moved methods reach the widgets and the model through it (a pure
    // move keeps the bodies as they were).
    void SetWindow(MainWindow* win) { fWin = win; }

    // The window's engine work, body for body (M1.1).
    void StartPlayback();
    void StopPlayback(bool resumeMonitor = true);   // false when about to record
    void ReloadActiveEngine();               // rebuild the running engine
    bool StartRecordEngine(Frame engineStart);   // engine for overdub monitoring

    // The engine, created on first use. The window owns it from then on: it is
    // deliberately NOT re-created per play (the device stays open).
    Engine* EnsureEngine();

    // Polled from the window's 60 Hz pulse: a build that failed on the worker
    // stops the transport here, with the same report the synchronous load used
    // to make. Nothing to do while every load succeeds.
    void PollEngineLoad();

    // The play range for the current transport state: to the loop end when
    // looping, plus the metronome-only extension the old Load call computed.
    Frame PlayRangeEnd(bool looping) const;

    std::unique_ptr<Engine> fEngine;
    bool        fPlaying = false;
    bool        fMonitoring = false;   // idle live-monitor engine up
    size_t      fBufferFrames = 512;   // output buffer frames/channel
    bool        fMetronome = false;
    bool        fMonDim = false;
    bool        fMonMono = false;
    bool        fMonitorInput = false; // hear live input while armed

private:
    MainWindow* fWin = nullptr;
    // LoadsCompleted() as of the last poll, so a completed build is noticed
    // once. Reset whenever a NEW engine object takes over.
    uint64_t    fSeenLoads = 0;
};

} // namespace daw
