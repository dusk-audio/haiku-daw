// Shared layout constants + a few color helpers for the timeline UI.
// Kept in one header so the ruler, lanes, and headers agree on geometry.
#pragma once

#include <InterfaceDefs.h>

namespace daw {

// Vertical layout (pixels).
constexpr float kRulerHeight  = 28.0f;   // top time-ruler strip
constexpr float kTrackHeight  = 74.0f;   // one track lane
constexpr float kTrackGap     = 1.0f;    // divider between lanes
constexpr float kHeaderWidth  = 210.0f;  // left track-header column (incl. meter + In box)
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

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

// Palette (dark, Logic-style). Neutral graphite greys with vivid track-colored
// regions; used across timeline + mixer + dialogs so nothing falls back to the
// light OS default.
inline rgb_color ColBackground() { return Rgb(30, 31, 34); }
inline rgb_color ColLane()       { return Rgb(58, 60, 64); }   // lane (neutral)
inline rgb_color ColLaneAlt()    { return Rgb(52, 54, 58); }
inline rgb_color ColRuler()      { return Rgb(38, 39, 43); }
inline rgb_color ColGrid()       { return Rgb(74, 77, 83); }
inline rgb_color ColClip()       { return Rgb(74, 120, 172); }
inline rgb_color ColClipBorder() { return Rgb(150, 190, 232); }
inline rgb_color ColWave()       { return Rgb(232, 240, 250); }
inline rgb_color ColText()       { return Rgb(222, 225, 230); }
inline rgb_color ColTextDim()    { return Rgb(150, 154, 162); }
inline rgb_color ColPlayhead()   { return Rgb(240, 240, 240); } // Logic white head
inline rgb_color ColHeader()     { return Rgb(48, 50, 54); }
inline rgb_color ColHeaderHi()   { return Rgb(66, 69, 74); }   // raised control
inline rgb_color ColAccent()     { return Rgb(74, 144, 217); } // Logic blue

// Control-bar / mixer chrome.
inline rgb_color ColChrome()     { return Rgb(42, 43, 47); }   // bar background
inline rgb_color ColChromeHi()   { return Rgb(60, 62, 67); }
inline rgb_color ColLcd()        { return Rgb(20, 22, 25); }   // LCD display well
inline rgb_color ColLcdText()    { return Rgb(228, 232, 238); }

// A level -> meter color (green below -6 dBish, yellow, red near clip).
inline rgb_color MeterColor(float level) {
    if (level >= 0.90f) return Rgb(232, 72, 60);    // red (near/over 0 dBFS)
    if (level >= 0.60f) return Rgb(226, 190, 66);   // yellow
    return Rgb(78, 190, 120);                        // green
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
