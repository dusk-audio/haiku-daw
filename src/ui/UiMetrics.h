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

// Vertical layout. The values are DESIGN pixels, authored at
// kDesignFontSize (Theme.h); every accessor goes through Themed(), so the
// user's font size scales the whole arrangement view. Accessors, not
// constants: an unscaled copy is the bug this exists to prevent.
constexpr float kDesignRulerHeight = 28.0f;  // top time-ruler strip
constexpr float kDesignTrackHeight = 74.0f;  // one track lane
constexpr float kDesignTrackGap    = 1.0f;   // divider between lanes
constexpr float kDesignHeaderWidth = 150.0f; // slim per-lane header
constexpr float kDesignHdrMeterW   = 16.0f;  // per-track meter strip (right edge)
constexpr float kDesignInspectorW  = 190.0f; // left inspector column
constexpr float kDesignTransportH  = 36.0f;  // the transport strip
// The arrange tool strip (M2.3), above the ruler: six tool buttons at the
// piano roll's button pitch, then the snap field and the zoom pair.
constexpr float kDesignToolbarHeight = 28.0f;
// The strip's own width: what the timeline must be able to show without
// clipping a button (MainWindow's minimum for the pane).
constexpr float kDesignArrToolStripW = 288.0f;

inline float RulerHeight()    { return Themed(kDesignRulerHeight); }
inline float ToolbarHeight()  { return Themed(kDesignToolbarHeight); }
inline float TrackHeight()    { return Themed(kDesignTrackHeight); }
inline float TrackGap()       { return Themed(kDesignTrackGap); }
inline float HeaderWidth()    { return Themed(kDesignHeaderWidth); }
inline float HdrMeterW()      { return Themed(kDesignHdrMeterW); }
inline float InspectorWidth() { return Themed(kDesignInspectorW); }
inline float ArrToolStripWidth() { return Themed(kDesignArrToolStripW); }

// Top of the arrange view's lane area: the tool strip, then the ruler. The
// timeline's own ContentTop() and every test that computes a lane's y use this
// one, so a click target and the drawn lane cannot disagree.
inline float TimelineContentTop() { return ToolbarHeight() + RulerHeight(); }

// Horizontal zoom default: frames represented by one pixel. 48000/px ~= 1s/px.
constexpr double kDefaultFramesPerPixel = 512.0;

// The modifier state for the event being handled. Mouse and key messages carry
// "modifiers" (the input server adds it); a mouse WHEEL message does not, so the
// live keyboard state is the fallback -- and a synthetic event from the
// functional test can set the field to drive either path.
inline uint32 EventModifiers(const BMessage* msg) {
    int32 mods = 0;
    if (msg && msg->FindInt32("modifiers", &mods) == B_OK)
        return (uint32)mods;
    return modifiers();
}

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

// Inspector -> MainWindow: toggle the global input monitor (same as the View
// menu item; shares the fourcc so the existing handler catches it).
constexpr uint32 kMsgInputMon = 'moni';

// Inspector -> MainWindow: cycle the selected track's automation lane mode
// (Off -> Gain -> Pan -> fx lanes). The timeline owns the mode + curve editing.
constexpr uint32 kMsgCycleAuto = 'caut';

// TimelineView -> MainWindow: open the MIDI editor on a region (M1.4). The
// editor lives in the docked bottom pane; the main window decides whether it
// is docked or popped out into its own window.
// int64 "track", int64 "clip".
constexpr uint32 kMsgOpenEditor = 'oped';

// Any view -> MainWindow: show the (single) effects window on a track's chain
// (T2). Fields: int64 "track" (the master chain for the master sentinel),
// int32 "focus" (insert index, or -1 for the whole chain). It lives here, with
// the other cross-view messages, because the inspector and the channel strip
// send it and neither includes MainWindow.h.
constexpr uint32 kMsgShowFx = 'shfx';

// TimelineView -> MainWindow: a clip/region edit (move/resize/fade/gain/split)
// committed; rebuild the running engine at the playhead so it takes effect live
// (clip fades, positions, etc. are otherwise only read at Load).
constexpr uint32 kMsgReloadEngine = 'reld';

} // namespace daw
