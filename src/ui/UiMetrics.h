// Shared layout constants + a few color helpers for the timeline UI.
// Kept in one header so the ruler, lanes, and headers agree on geometry.
#pragma once

#include <InterfaceDefs.h>

namespace daw {

// Vertical layout (pixels).
constexpr float kRulerHeight  = 28.0f;   // top time-ruler strip
constexpr float kTrackHeight  = 74.0f;   // one track lane
constexpr float kTrackGap     = 1.0f;    // divider between lanes
constexpr float kHeaderWidth  = 178.0f;  // left track-header column (incl. meter)
constexpr float kHdrMeterW    = 16.0f;   // per-track meter strip width (right edge)

// Horizontal zoom default: frames represented by one pixel. 48000/px ~= 1s/px.
constexpr double kDefaultFramesPerPixel = 512.0;

// TimelineView -> MainWindow: user clicked the ruler to move the playhead.
constexpr uint32 kMsgSeek = 'seek';

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

// Palette (dark, DAW-ish). Cohesive blue-grey; used across timeline + mixer +
// dialogs so nothing falls back to the light OS default.
inline rgb_color ColBackground() { return Rgb(24, 26, 31); }
inline rgb_color ColLane()       { return Rgb(37, 40, 47); }
inline rgb_color ColLaneAlt()    { return Rgb(32, 35, 41); }
inline rgb_color ColRuler()      { return Rgb(18, 20, 24); }
inline rgb_color ColGrid()       { return Rgb(52, 56, 64); }
inline rgb_color ColClip()       { return Rgb(64, 108, 160); }
inline rgb_color ColClipBorder() { return Rgb(128, 176, 226); }
inline rgb_color ColWave()       { return Rgb(198, 220, 244); }
inline rgb_color ColText()       { return Rgb(214, 218, 226); }
inline rgb_color ColTextDim()    { return Rgb(140, 146, 158); }
inline rgb_color ColPlayhead()   { return Rgb(236, 96, 74); }
inline rgb_color ColHeader()     { return Rgb(41, 44, 52); }
inline rgb_color ColHeaderHi()   { return Rgb(52, 56, 66); }   // raised control
inline rgb_color ColAccent()     { return Rgb(90, 150, 220); } // active/selected

// A level -> meter color (green below -6 dBish, yellow, red near clip).
inline rgb_color MeterColor(float level) {
    if (level >= 0.90f) return Rgb(232, 72, 60);    // red (near/over 0 dBFS)
    if (level >= 0.60f) return Rgb(226, 190, 66);   // yellow
    return Rgb(78, 190, 120);                        // green
}

// Per-track color palette (Track::colorIndex). Index 0 = the default clip blue.
inline rgb_color TrackColor(int i) {
    static const rgb_color pal[] = {
        Rgb(64, 108, 160),   // blue (default)
        Rgb(160, 84, 84),    // red
        Rgb(84, 150, 90),    // green
        Rgb(150, 120, 70),   // amber
        Rgb(120, 90, 160),   // purple
        Rgb(70, 140, 150),   // teal
    };
    const int n = (int)(sizeof(pal) / sizeof(pal[0]));
    if (i < 0) i = 0;
    return pal[i % n];
}
constexpr int kTrackColorCount = 6;

} // namespace daw
