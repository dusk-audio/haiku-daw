// Shared layout constants + a few color helpers for the timeline UI.
// Kept in one header so the ruler, lanes, and headers agree on geometry.
#pragma once

#include <InterfaceDefs.h>

namespace daw {

// Vertical layout (pixels).
constexpr float kRulerHeight  = 28.0f;   // top time-ruler strip
constexpr float kTrackHeight  = 72.0f;   // one track lane
constexpr float kTrackGap     = 2.0f;    // divider between lanes
constexpr float kHeaderWidth  = 160.0f;  // left track-header column

// Horizontal zoom default: frames represented by one pixel. 48000/px ~= 1s/px.
constexpr double kDefaultFramesPerPixel = 512.0;

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

// Palette (dark, DAW-ish).
inline rgb_color ColBackground() { return Rgb(30, 32, 36); }
inline rgb_color ColLane()       { return Rgb(38, 41, 46); }
inline rgb_color ColLaneAlt()    { return Rgb(34, 37, 42); }
inline rgb_color ColRuler()      { return Rgb(22, 24, 27); }
inline rgb_color ColGrid()       { return Rgb(55, 59, 66); }
inline rgb_color ColClip()       { return Rgb(64, 108, 160); }
inline rgb_color ColClipBorder() { return Rgb(120, 170, 220); }
inline rgb_color ColWave()       { return Rgb(190, 214, 240); }
inline rgb_color ColText()       { return Rgb(210, 214, 220); }
inline rgb_color ColPlayhead()   { return Rgb(230, 90, 70); }
inline rgb_color ColHeader()     { return Rgb(44, 47, 53); }

} // namespace daw
