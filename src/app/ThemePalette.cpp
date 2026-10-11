// ThemePalette — see the header. Kit-free, pure, host-tested.
#include "ThemePalette.h"

#include <cstring>

namespace daw {

namespace {

// The vivid colours that read on a light and a dark panel alike: a clip is
// blue in any DAW, a meter turns yellow at the same place, an armed track is
// red. Deriving these from the panel colour would make the DAW change
// character with the user's Appearance, and every one of them is either drawn
// with an auto-contrast label (LabelOn) or sits on its own dark well.
constexpr ThemeColor kClip        = {0, 122, 255};
constexpr ThemeColor kClipBorder  = {120, 180, 255};
constexpr ThemeColor kWave        = {232, 240, 250};
constexpr ThemeColor kMute        = {255, 204, 0};
constexpr ThemeColor kSolo        = {0, 176, 255};
constexpr ThemeColor kRec         = {255, 59, 48};
constexpr ThemeColor kMon         = {255, 149, 0};
constexpr ThemeColor kPlay        = {52, 199, 89};
constexpr ThemeColor kMidiAccent  = {52, 199, 89};

} // namespace

ThemeBase ThemeBase::Defaults() {
    // Haiku's own defaults (InterfaceDefs.cpp's _kDefaultColors), so a call
    // before the app is up — or a test — sees the shipped light Appearance.
    ThemeBase b;
    b.panel                  = Rgb8(216, 216, 216);
    b.panelText              = Rgb8(0, 0, 0);
    b.document               = Rgb8(255, 255, 255);
    b.documentText           = Rgb8(0, 0, 0);
    b.control                = Rgb8(222, 222, 222);
    b.controlText            = Rgb8(0, 0, 0);
    b.controlBorder          = Rgb8(172, 172, 172);
    b.menuBackground         = Rgb8(216, 216, 216);
    b.menuItemText           = Rgb8(0, 0, 0);
    b.menuSelectedBackground = Rgb8(153, 153, 153);
    b.menuSelectedItemText   = Rgb8(0, 0, 0);
    b.scrollBarThumb         = Rgb8(216, 216, 216);
    b.listBackground         = Rgb8(255, 255, 255);
    b.listSelectedBackground = Rgb8(190, 190, 190);
    b.listItemText           = Rgb8(0, 0, 0);
    b.listSelectedItemText   = Rgb8(0, 0, 0);
    b.keyboardNavigation     = Rgb8(0, 0, 229);
    return b;
}

ThemeTokens DeriveTokens(const ThemeBase& base, ThemeMode mode) {
    ThemeTokens t;

    if (mode == ThemeMode::Dark) {
        // The palette the app has drawn with since M1.2 — unchanged, so this
        // change is a mode switch and not a redesign of the dark look.
        t.background  = Rgb8(30, 30, 36);
        t.lane        = Rgb8(36, 36, 42);
        t.laneAlt     = Rgb8(31, 31, 37);
        t.ruler       = Rgb8(26, 26, 31);
        t.grid        = Rgb8(20, 20, 24);
        t.clip        = Rgb8(0, 122, 255);
        t.clipBorder  = Rgb8(120, 180, 255);
        t.wave        = Rgb8(232, 240, 250);
        t.text        = Rgb8(224, 224, 224);
        t.textDim     = Rgb8(142, 142, 147);
        t.playhead    = Rgb8(240, 240, 240);
        t.header      = Rgb8(38, 38, 44);
        t.headerHi    = Rgb8(50, 50, 58);
        t.accent      = Rgb8(0, 122, 255);
        t.audioAccent = Rgb8(0, 122, 255);
        t.midiAccent  = Rgb8(52, 199, 89);
        t.btnOff      = Rgb8(58, 58, 64);
        t.btnBorder   = Rgb8(74, 74, 80);
        t.btnText     = Rgb8(160, 160, 160);
        t.mute        = Rgb8(255, 204, 0);
        t.solo        = Rgb8(0, 176, 255);
        t.rec         = Rgb8(255, 59, 48);
        t.mon         = Rgb8(255, 149, 0);
        t.play        = Rgb8(52, 199, 89);
        t.knobBody    = Rgb8(44, 44, 52);
        t.knobOutline = Rgb8(72, 72, 82);
        t.chrome      = Rgb8(30, 30, 36);
        t.chromeHi    = Rgb8(50, 50, 58);
        t.lcd         = Rgb8(15, 16, 21);
        t.lcdText     = Rgb8(0, 240, 255);
        return t;
    }

    // --- System: the user's own colours, and the stock look -----------------
    //
    // Every rule here has to hold for a light panel (Haiku's default 216,216,216)
    // and a dark one (43,43,43), because the Appearance setting is the user's.
    // The way that works without a branch per token: derive each variant from
    // the base colour it belongs to, and let IsDark decide which way "variant"
    // goes. theme_palette_tests asserts the contrast these produce.
    const bool dark = IsDark(base.panel);

    t.background = base.panel;
    t.chrome     = base.panel;
    t.text       = base.panelText;
    t.textDim    = Mix(base.panelText, base.panel, 0.45f);

    // Header strips and recessed wells: Haiku's own relationship between the
    // panel and the control/document colours (light: controls sit above the
    // panel; dark: below it) is exactly what a lane header wants.
    t.header   = base.control;
    t.headerHi = dark ? Tint(base.control, 1.4f) : Tint(base.control, 0.88f);
    t.chromeHi = t.headerHi;
    t.ruler    = Tint(base.panel, dark ? 0.82f : 0.95f);
    t.grid     = Mix(base.panel, base.panelText, 0.25f);

    t.lane    = base.listBackground;
    t.laneAlt = dark ? Tint(t.lane, 1.12f) : Tint(t.lane, 0.96f);

    t.clip       = kClip;
    t.clipBorder = kClipBorder;
    t.wave       = kWave;
    // The playhead has to read on the panel it sweeps across, so it is the
    // text colour's problem, not a constant: near-white on a dark panel,
    // near-black on a light one.
    t.playhead = dark ? Rgb8(240, 240, 240) : Rgb8(25, 25, 30);

    t.accent      = EnsureContrast(base.keyboardNavigation, base.panel, 3.0f);
    t.audioAccent = t.accent;
    // MIDI green is a label colour as often as a fill, so it too has to carry
    // on the panel it is drawn on.
    t.midiAccent = EnsureContrast(kMidiAccent, base.panel, 3.0f);

    t.btnOff    = base.control;
    t.btnBorder = base.controlBorder;
    t.btnText   = base.controlText;

    t.mute = kMute;
    t.solo = kSolo;
    t.rec  = kRec;
    t.mon  = kMon;
    t.play = kPlay;

    t.knobBody    = dark ? Tint(base.control, 1.4f) : Tint(base.control, 0.82f);
    t.knobOutline = base.controlBorder;

    // A recessed well is the document colour in Haiku, not a black hole: that
    // is what keeps a text field looking like a text field in System mode.
    t.lcd     = base.document;
    t.lcdText = base.documentText;

    return t;
}

ThemeMode ThemeModeFromString(const char* text) {
    if (text != nullptr && std::strcmp(text, "dark") == 0)
        return ThemeMode::Dark;
    return ThemeMode::System;
}

const char* ThemeModeToString(ThemeMode mode) {
    return mode == ThemeMode::Dark ? "dark" : "system";
}

} // namespace daw
