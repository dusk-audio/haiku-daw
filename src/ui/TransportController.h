// TransportController — the engine's lifetime and the transport's rolling
// state, moved out of MainWindow (M1.1, slice 2).
//
// Public members on purpose: this slice is a pure move, and the window still
// drives every one of them. The method moves (play, stop, rebuild, seek,
// loop) land on top of this in the same item, tightening the seams as they go;
// keeping the state in one place first is what makes those moves mechanical.
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

    // Rebuilt per Play today (RT-safe graph swap later).
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
};

} // namespace daw
