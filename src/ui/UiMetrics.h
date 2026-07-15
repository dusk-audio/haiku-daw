// Shared layout constants + a few color helpers for the timeline UI.
// Kept in one header so the ruler, lanes, and headers agree on geometry.
#pragma once

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

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

// Palette — "Logic Slate" dark theme. Dark slate panels, near-black recesses,
// blue (audio) / green (MIDI) accents. Used across timeline + inspector + mixer
// + dialogs so nothing falls back to the light OS default.
inline rgb_color ColBackground() { return Rgb(30, 30, 36); }   // #1e1e24 window
inline rgb_color ColLane()       { return Rgb(36, 36, 42); }   // lane row
inline rgb_color ColLaneAlt()    { return Rgb(31, 31, 37); }   // alt lane row
inline rgb_color ColRuler()      { return Rgb(26, 26, 31); }
inline rgb_color ColGrid()       { return Rgb(20, 20, 24); }   // #141418 borders/dividers
inline rgb_color ColClip()       { return Rgb(0, 122, 255); }  // audio region (blue)
inline rgb_color ColClipBorder() { return Rgb(120, 180, 255); }
inline rgb_color ColWave()       { return Rgb(232, 240, 250); }
inline rgb_color ColText()       { return Rgb(224, 224, 224); } // #e0e0e0
inline rgb_color ColTextDim()    { return Rgb(142, 142, 147); } // #8e8e93
inline rgb_color ColPlayhead()   { return Rgb(240, 240, 240); }
inline rgb_color ColHeader()     { return Rgb(38, 38, 44); }   // #26262c panel
inline rgb_color ColHeaderHi()   { return Rgb(50, 50, 58); }   // #32323a selected/raised
inline rgb_color ColAccent()     { return Rgb(0, 122, 255); }  // #007aff Logic blue

// Track-type accents.
inline rgb_color ColAudioAccent(){ return Rgb(0, 122, 255); }  // #007aff
inline rgb_color ColMidiAccent() { return Rgb(52, 199, 89); }  // #34c759

// Button states (rounded M/S/R/I).
inline rgb_color ColBtnOff()     { return Rgb(58, 58, 64); }   // #3a3a40
inline rgb_color ColBtnBorder()  { return Rgb(74, 74, 80); }   // #4a4a50
inline rgb_color ColBtnText()    { return Rgb(160, 160, 160); }// #a0a0a0
inline rgb_color ColMute()       { return Rgb(255, 204, 0); }  // #ffcc00 (black text)
inline rgb_color ColSolo()       { return Rgb(0, 176, 255); }  // #00b0ff (black text)
inline rgb_color ColRec()        { return Rgb(255, 59, 48); }  // #ff3b30 (white text)
inline rgb_color ColMon()        { return Rgb(255, 149, 0); }  // #ff9500 (black text)
inline rgb_color ColPlay()       { return Rgb(52, 199, 89); }  // #34c759

// Knob.
inline rgb_color ColKnobBody()   { return Rgb(44, 44, 52); }   // #2c2c34
inline rgb_color ColKnobOutline(){ return Rgb(72, 72, 82); }   // #484852

// Control-bar / mixer chrome.
inline rgb_color ColChrome()     { return Rgb(30, 30, 36); }   // bar background
inline rgb_color ColChromeHi()   { return Rgb(50, 50, 58); }
inline rgb_color ColLcd()        { return Rgb(15, 16, 21); }   // #0f1015 recessed well
inline rgb_color ColLcdText()    { return Rgb(0, 240, 255); }  // #00f0ff glowing cyan

// A level -> meter color (green below -6 dBish, yellow, red near clip).
inline rgb_color MeterColor(float level) {
    if (level >= 0.90f) return Rgb(255, 59, 48);    // red #ff3b30 (near/over 0)
    if (level >= 0.60f) return Rgb(255, 204, 0);    // yellow #ffcc00
    return Rgb(52, 199, 89);                         // green #34c759
}

// Per-track color palette (Track::colorIndex). Index 0 = the default clip blue.
inline rgb_color TrackColor(int i) {
    // Vivid, Logic-like region colors.
    static const rgb_color pal[] = {
        Rgb(74, 144, 217),   // blue (default)
        Rgb(224, 85, 154),   // pink
        Rgb(92, 184, 92),    // green
        Rgb(232, 147, 58),   // orange
        Rgb(155, 108, 212),  // purple
        Rgb(64, 178, 188),   // teal
    };
    const int n = (int)(sizeof(pal) / sizeof(pal[0]));
    if (i < 0) i = 0;
    return pal[i % n];
}
constexpr int kTrackColorCount = 6;

} // namespace daw
