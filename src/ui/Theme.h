// Theme.h — the UI's one place for colour, type, scale and the meter
// thresholds (plan M1.2). UiMetrics.h grew into this: the layout constants and
// the shared message ids stay there, and it includes this header, so every
// existing include keeps working.
//
// The colours are the tokens; ThemeFontSize()/ThemeScale() are how a user's
// font-size preference reaches the layout (the release checklist's 150% pass),
// authored against kDesignFontSize below.
#pragma once

#include <AppDefs.h>
#include <Font.h>
#include <InterfaceDefs.h>

namespace daw {

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

// --- type ---------------------------------------------------------------

// Every metric in the UI is authored against this size; the scale is the
// ratio of the user's plain font to it.
constexpr float kDesignFontSize = 12.0f;

inline float ThemeFontSize() {
    // Cached: be_plain_font is set up with the BApplication, before any view
    // asks, and a font change mid-session is a restart in Haiku.
    static const float size = [] {
        float s = be_plain_font ? be_plain_font->Size() : kDesignFontSize;
        if (s < 8.0f) s = 8.0f;
        if (s > 48.0f) s = 48.0f;
        return s;
    }();
    return size;
}

inline float ThemeScale() { return ThemeFontSize() / kDesignFontSize; }

// A design metric in device pixels.
inline float Themed(float v) { return v * ThemeScale(); }

inline BFont ThemeFont() {
    BFont f(be_plain_font);
    f.SetSize(ThemeFontSize());
    return f;
}

inline BFont ThemeFontBold() {
    BFont f(be_bold_font);
    f.SetSize(ThemeFontSize());
    return f;
}

// --- meter thresholds (ONE set: MeterView and MeterColor disagreed) ------

// -3 dBFS-ish and 0: where a meter turns yellow, and where it turns red.
constexpr float kMeterWarn = 0.70f;
constexpr float kMeterClip = 1.00f;

// A level -> meter color (green below the warn threshold, yellow, red at
// clip). The one place a meter's colours come from.
inline rgb_color MeterColor(float level) {
    if (level >= kMeterClip) return Rgb(255, 59, 48);   // red #ff3b30 (at/over 0)
    if (level >= kMeterWarn) return Rgb(255, 204, 0);   // yellow #ffcc00
    return Rgb(52, 199, 89);                            // green #34c759
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
