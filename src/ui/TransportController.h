// TransportController — the engine's lifetime and the transport's rolling
// state, moved out of MainWindow (M1.1, slice 2).
//
// Public members on purpose: this slice is a pure move, and the window still
// drives every one of them. The method moves (play, stop, rebuild, seek,
// loop) land on top of this in the same item, tightening the seams as they go;
// keeping the state in one place first is what makes those moves mechanical.
#pragma once

#include "../engine/Engine.h"

#include <memory>

namespace daw {

class TransportController {
public:
    // Rebuilt per Play today (RT-safe graph swap later).
    std::unique_ptr<Engine> fEngine;
    bool        fPlaying = false;
    bool        fMonitoring = false;   // idle live-monitor engine up
    size_t      fBufferFrames = 512;   // output buffer frames/channel
    bool        fMetronome = false;
    bool        fMonDim = false;
    bool        fMonMono = false;
    bool        fMonitorInput = false; // hear live input while armed
};

} // namespace daw
