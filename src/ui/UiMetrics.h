// Shared layout constants and the messages the UI's pieces send each other.
// Kept in one header so the ruler, lanes and headers agree on geometry; the
// colours, type and meter thresholds live in Theme.h (which this includes, so
// every existing include of this file keeps working).
#pragma once

#include "Theme.h"

#include <AppDefs.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <TypeConstants.h>

namespace daw {

// Vertical layout (pixels).
constexpr float kRulerHeight  = 28.0f;   // top time-ruler strip
constexpr float kTrackHeight  = 74.0f;   // one track lane
constexpr float kTrackGap     = 1.0f;    // divider between lanes
constexpr float kHeaderWidth  = 150.0f;  // slim per-lane header (full controls in the inspector)
constexpr float kHdrMeterW    = 16.0f;   // per-track meter strip width (right edge)

// Horizontal zoom default: frames represented by one pixel. 48000/px ~= 1s/px.
constexpr double kDefaultFramesPerPixel = 512.0;

// TimelineView -> MainWindow: user clicked the ruler to move the playhead.
constexpr uint32 kMsgSeek = 'seek';

// TimelineView -> MainWindow: arming or a track's input changed; re-evaluate
// idle MIDI monitoring (start/stop the monitor engine).
constexpr uint32 kMsgMonitorRefresh = 'mon?';

// TimelineView -> MainWindow: spacebar pressed; toggle play/stop.
constexpr uint32 kMsgTransportToggle = 'xptg';

// Forward a spacebar key-down to the main window as a transport toggle, so any
// child window (FX, Mixer, piano roll, ...) can start/stop playback. Call from a
// window's DispatchMessage; returns true if it consumed the event.
inline bool ForwardSpaceToTransport(BMessage* msg, const BMessenger& main) {
    if (!msg || msg->what != B_KEY_DOWN) return false;
    int32 repeat = 0;   // ignore auto-repeat so holding space doesn't spam toggle
    if (msg->FindInt32("be:key_repeat", &repeat) == B_OK && repeat > 1) return false;
    const char* bytes = nullptr;
    ssize_t len = 0;
    if (msg->FindData("bytes", B_STRING_TYPE, (const void**)&bytes, &len) == B_OK
        && len >= 1 && bytes[0] == ' ') {   // space
        main.SendMessage(kMsgTransportToggle);
        return true;
    }
    return false;
}

// TimelineView -> MainWindow: a track was selected (int64 "track"); the main
// window points the inspector at it.
constexpr uint32 kMsgTrackSelected = 'tsel';

// Either view -> MainWindow: a track edit happened; refresh both the timeline
// and the inspector so they stay consistent.
constexpr uint32 kMsgUiRefresh = 'uref';

// A channel strip -> MainWindow: an insert chain changed (added, reordered,
// bypassed) and the command has ALREADY run. This is what pushes the edit into
// the RUNNING engine.
//
// Not folded into kMsgUiRefresh, which is only a repaint: the timeline posts it
// for clip moves, gain writes and selection changes, and syncing the effect
// graph on each of those would rebuild the engine at the playhead for edits
// that have nothing to do with effects. The editor window does not need it
// either -- kMsgApplyFx carries a chain and syncs on the way past.
constexpr uint32 kMsgFxChanged = 'fxch';

// Left inspector column width (Logic-style track inspector).
constexpr float kInspectorWidth = 190.0f;

// Inspector -> MainWindow: toggle the global input monitor (same as the View
// menu item; shares the fourcc so the existing handler catches it).
constexpr uint32 kMsgInputMon = 'moni';

// Inspector -> MainWindow: cycle the selected track's automation lane mode
// (Off -> Gain -> Pan -> fx lanes). The timeline owns the mode + curve editing.
constexpr uint32 kMsgCycleAuto = 'caut';

// TimelineView -> MainWindow: a clip/region edit (move/resize/fade/gain/split)
// committed; rebuild the running engine at the playhead so it takes effect live
// (clip fades, positions, etc. are otherwise only read at Load).
constexpr uint32 kMsgReloadEngine = 'reld';

} // namespace daw
