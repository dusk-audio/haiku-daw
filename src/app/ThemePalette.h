// ThemePalette — the palette derivation, kit-free (plan T1: theme modes).
//
// `src/ui/Theme.h` used to *be* the palette: 30 inline functions returning the
// dark literals, compiled only on Haiku. Two modes need the colours to be
// derived from the user's system colours instead, and that derivation is the
// part worth testing — so it lives here, in `daw_model`, with no Haiku header
// in sight: base colours in, tokens out, pure functions.
//
// The three modes of a colour:
//   ThemeBase   — the 17 system colours, as data (filled from ui_color() by
//                 the Haiku side, hand-built in tests);
//   ThemeTokens — the colours the UI actually draws with, one per Theme.h
//                 accessor;
//   DeriveTokens(base, mode) — the only place the two are tied together.
#pragma once

#include <cmath>
#include <cstdint>

namespace daw {

// rgb_color's kit-free stand-in ([0,255] per channel).
struct ThemeColor {
    uint8_t r = 0, g = 0, b = 0;
};

constexpr ThemeColor Rgb8(uint8_t r, uint8_t g, uint8_t b) {
    return ThemeColor{r, g, b};
}

constexpr bool operator==(ThemeColor a, ThemeColor b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
constexpr bool operator!=(ThemeColor a, ThemeColor b) { return !(a == b); }

// --- colour maths --------------------------------------------------------

inline uint8_t Clamp8(float v) {
    if (!(v > 0.0f)) return 0;      // NaN and negatives -> 0
    if (v > 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

// Haiku's tint_color: a factor below 1 scales the channels down, above 1
// scales them up towards white. Same numbers so a System-mode variant sits on
// its base exactly the way a stock control's does.
inline ThemeColor Tint(ThemeColor c, float factor) {
    if (factor == 1.0f) return c;
    if (factor < 1.0f) {
        return Rgb8(Clamp8(c.r * factor), Clamp8(c.g * factor),
                    Clamp8(c.b * factor));
    }
    return Rgb8(Clamp8(255.0f - (255.0f - c.r) / factor),
                Clamp8(255.0f - (255.0f - c.g) / factor),
                Clamp8(255.0f - (255.0f - c.b) / factor));
}

// A blend: t = 0 gives `a`, t = 1 gives `b`.
inline ThemeColor Mix(ThemeColor a, ThemeColor b, float t) {
    if (!(t > 0.0f)) return a;
    if (t >= 1.0f) return b;
    return Rgb8(Clamp8(a.r + (b.r - a.r) * t), Clamp8(a.g + (b.g - a.g) * t),
                Clamp8(a.b + (b.b - a.b) * t));
}

// WCAG relative luminance and contrast ratio. These are what the host test
// asserts on, so they must be the real formulas, not a cheap approximation.
inline float LinearChannel(uint8_t v) {
    const float s = (float)v / 255.0f;
    return s <= 0.03928f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

inline float Luminance(ThemeColor c) {
    return 0.2126f * LinearChannel(c.r) + 0.7152f * LinearChannel(c.g)
         + 0.0722f * LinearChannel(c.b);
}

inline float ContrastRatio(ThemeColor a, ThemeColor b) {
    const float la = Luminance(a);
    const float lb = Luminance(b);
    const float hi = la > lb ? la : lb;
    const float lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

// Is this colour dark? (Used to pick which way a variant goes, so one rule
// serves a light and a dark system panel.)
inline bool IsDark(ThemeColor c) { return Luminance(c) < 0.5f; }

// Push `c` away from `background` until it contrasts with it. The user owns
// the accent colour, and Haiku keeps the SAME blue (0,0,229) in its dark
// Appearance as in its light one — so an accent taken at face value is
// invisible on one of the two panels. Focus rings and accent text are not
// decoration: they move until they read.
inline ThemeColor EnsureContrast(ThemeColor c, ThemeColor background,
                                 float minRatio) {
    const bool lighten = IsDark(background);
    ThemeColor out = c;
    for (int i = 0; i < 8 && ContrastRatio(out, background) < minRatio; i++)
        out = Tint(out, lighten ? 1.4f : 0.75f);
    return out;
}

// The label colour to put on `background`: whichever of the two reads better,
// which is the rule the clip and lamp drawing already used.
inline ThemeColor LabelOn(ThemeColor background) {
    return ContrastRatio(Rgb8(255, 255, 255), background)
               >= ContrastRatio(Rgb8(0, 0, 0), background)
           ? Rgb8(255, 255, 255)
           : Rgb8(0, 0, 0);
}

// --- the two modes -------------------------------------------------------

enum class ThemeMode {
    System = 0,   // the user's own colours and the stock control look
    Dark   = 1,   // the DAW's palette, in the DAW's windows only
};

// The system colours the DAW draws with — the same 17 the old
// ApplyThemeColors() overwrote (src/main.cpp), now read-only inputs. On Haiku
// these come from ui_color(); a test can pass any pair of panels it likes.
struct ThemeBase {
    ThemeColor panel;                  // B_PANEL_BACKGROUND_COLOR
    ThemeColor panelText;              // B_PANEL_TEXT_COLOR
    ThemeColor document;               // B_DOCUMENT_BACKGROUND_COLOR
    ThemeColor documentText;           // B_DOCUMENT_TEXT_COLOR
    ThemeColor control;                // B_CONTROL_BACKGROUND_COLOR
    ThemeColor controlText;            // B_CONTROL_TEXT_COLOR
    ThemeColor controlBorder;          // B_CONTROL_BORDER_COLOR
    ThemeColor menuBackground;         // B_MENU_BACKGROUND_COLOR
    ThemeColor menuItemText;           // B_MENU_ITEM_TEXT_COLOR
    ThemeColor menuSelectedBackground; // B_MENU_SELECTED_BACKGROUND_COLOR
    ThemeColor menuSelectedItemText;   // B_MENU_SELECTED_ITEM_TEXT_COLOR
    ThemeColor scrollBarThumb;         // B_SCROLL_BAR_THUMB_COLOR
    ThemeColor listBackground;         // B_LIST_BACKGROUND_COLOR
    ThemeColor listSelectedBackground; // B_LIST_SELECTED_BACKGROUND_COLOR
    ThemeColor listItemText;           // B_LIST_ITEM_TEXT_COLOR
    ThemeColor listSelectedItemText;   // B_LIST_SELECTED_ITEM_TEXT_COLOR
    ThemeColor keyboardNavigation;     // B_KEYBOARD_NAVIGATION_COLOR (accent)

    // Haiku's shipped defaults (Appearance > Colors > Defaults), so a run with
    // no Appearance data at all — a test, or a call before the app is up —
    // still gets a sane light palette rather than black.
    static ThemeBase Defaults();
};

// The colours the UI draws with: one field per Theme.h accessor.
struct ThemeTokens {
    ThemeColor background, lane, laneAlt, ruler, grid;
    ThemeColor clip, clipBorder, wave;
    ThemeColor text, textDim, playhead;
    ThemeColor header, headerHi, accent;
    ThemeColor audioAccent, midiAccent;
    ThemeColor btnOff, btnBorder, btnText;
    ThemeColor mute, solo, rec, mon, play;
    ThemeColor knobBody, knobOutline;
    ThemeColor chrome, chromeHi, lcd, lcdText;
};

// The derivation. Pure: same base + mode -> same tokens, which is what makes
// the contrast assertions in theme_palette_tests meaningful.
ThemeTokens DeriveTokens(const ThemeBase& base, ThemeMode mode);

// The mode from its settings text ("system" / "dark"); anything else is
// System, so a hand-edited settings file can never leave the app dark.
ThemeMode ThemeModeFromString(const char* text);
const char* ThemeModeToString(ThemeMode mode);

} // namespace daw
