// Theme.h — the UI's one place for colour, type, scale and the meter
// thresholds (plan M1.2), and since T1 the two theme modes.
//
// The colours are TOKENS derived by the kit-free palette (src/app/
// ThemePalette.h) from the user's system colours (System mode) or from the
// DAW's own dark palette (Dark mode). Accessors, so every call site keeps
// working — and so a mode change reaches the screen without a rebuild.
//
// What a mode change needs from a view: the tokens it *draws* with are read
// through these accessors at draw time and are always current. A colour it
// *cached* — SetViewColor/SetLowColor in a constructor, a kit control's
// adopted panel colour — has to be re-taken, which is what ThemeAware is for
// and what ApplyThemeToAllWindows() walks the window tree to deliver.
#pragma once

#include "../app/ThemePalette.h"   // the derivation (kit-free, host-tested)

#include <Alert.h>
#include <Application.h>
#include <AppDefs.h>
#include <Font.h>
#include <InterfaceDefs.h>
#include <GroupView.h>
#include <ListView.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

namespace daw {

inline rgb_color Rgb(uint8 r, uint8 g, uint8 b) {
    return rgb_color{r, g, b, 255};
}

inline rgb_color ToRgb(ThemeColor c) {
    return rgb_color{c.r, c.g, c.b, 255};
}

// Two blends for the shared drawings: a fader cap or a hover tint is built
// from the theme's colours instead of hard-coded greys (T1). The maths is the
// kit-free palette's, so a host test covers it.
inline rgb_color MixColor(rgb_color a, rgb_color b, float t) {
    return ToRgb(Mix(ThemeColor{a.red, a.green, a.blue},
                     ThemeColor{b.red, b.green, b.blue}, t));
}

inline rgb_color TintColor(rgb_color c, float factor) {
    return ToRgb(Tint(ThemeColor{c.red, c.green, c.blue}, factor));
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

// Test hook (ui_functional_tests): force the scale, so a 150% layout can be
// exercised on the VM where the font size cannot change mid-run. 0 = derive
// from the font. The font tokens stay the real font: this scales METRICS, and
// its job is to prove that layout and hit-testing agree at a non-100% scale.
inline float& ThemeScaleOverride() { static float s = 0.0f; return s; }
inline void SetThemeScaleOverride(float s) { ThemeScaleOverride() = s; }

inline float ThemeScale() {
    const float forced = ThemeScaleOverride();
    if (forced > 0.0f) return forced;
    return ThemeFontSize() / kDesignFontSize;
}

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

// --- the active theme ----------------------------------------------------

// The process's one theme: the mode, the system colours it was derived from,
// and the tokens those produced. System + Haiku's shipped defaults until the
// application installs the user's (DawApplication::ReadyToRun) — so a call
// that arrives early draws a sane light panel, never black.
struct ActiveTheme {
    ThemeMode   mode   = ThemeMode::System;
    ThemeBase   base   = ThemeBase::Defaults();
    ThemeTokens tokens = DeriveTokens(base, mode);

    void Recompute() { tokens = DeriveTokens(base, mode); }
};

inline ActiveTheme& ThemeState() {
    static ActiveTheme state;
    return state;
}

inline const ThemeTokens& Tokens() { return ThemeState().tokens; }
inline ThemeMode ActiveThemeMode() { return ThemeState().mode; }

inline void SetActiveThemeMode(ThemeMode mode) {
    ThemeState().mode = mode;
    ThemeState().Recompute();
}

// The user's system colours (System mode's inputs). Called by the application
// at startup and whenever Haiku reports B_COLORS_UPDATED.
inline void SetSystemBaseColors(const ThemeBase& base) {
    ThemeState().base = base;
    if (ThemeState().mode == ThemeMode::System)
        ThemeState().Recompute();
}

// The user's system colours, read from Haiku. Only System mode uses them, but
// they are kept current either way so a switch to System never shows a stale
// Appearance. Called at startup and on B_COLORS_UPDATED.
inline void ReadSystemBaseColors() {
    auto read = [](color_which which) {
        const rgb_color c = ui_color(which);
        return ThemeColor{c.red, c.green, c.blue};
    };
    ThemeBase b;
    b.panel                  = read(B_PANEL_BACKGROUND_COLOR);
    b.panelText              = read(B_PANEL_TEXT_COLOR);
    b.document               = read(B_DOCUMENT_BACKGROUND_COLOR);
    b.documentText           = read(B_DOCUMENT_TEXT_COLOR);
    b.control                = read(B_CONTROL_BACKGROUND_COLOR);
    b.controlText            = read(B_CONTROL_TEXT_COLOR);
    b.controlBorder          = read(B_CONTROL_BORDER_COLOR);
    b.menuBackground         = read(B_MENU_BACKGROUND_COLOR);
    b.menuItemText           = read(B_MENU_ITEM_TEXT_COLOR);
    b.menuSelectedBackground = read(B_MENU_SELECTED_BACKGROUND_COLOR);
    b.menuSelectedItemText   = read(B_MENU_SELECTED_ITEM_TEXT_COLOR);
    b.scrollBarThumb         = read(B_SCROLL_BAR_THUMB_COLOR);
    b.listBackground         = read(B_LIST_BACKGROUND_COLOR);
    b.listSelectedBackground = read(B_LIST_SELECTED_BACKGROUND_COLOR);
    b.listItemText           = read(B_LIST_ITEM_TEXT_COLOR);
    b.listSelectedItemText   = read(B_LIST_SELECTED_ITEM_TEXT_COLOR);
    b.keyboardNavigation     = read(B_KEYBOARD_NAVIGATION_COLOR);
    SetSystemBaseColors(b);
}

// --- refresh -------------------------------------------------------------

// A view or window that CACHED a theme colour re-takes it here. Views that
// draw with the tokens need nothing: the walker invalidates them.
class ThemeAware {
public:
    virtual ~ThemeAware() = default;
    virtual void ApplyTheme() = 0;
};

inline void ApplyThemeToView(BView* view) {
    if (view == nullptr) return;
    if (auto* aware = dynamic_cast<ThemeAware*>(view))
        aware->ApplyTheme();
    for (int32 i = 0; i < view->CountChildren(); i++)
        ApplyThemeToView(view->ChildAt(i));
    view->Invalidate();
}

// Every direct child of a window. NOT ChildAt(0): a window built with the
// Layout Kit has one child per top-level item (the menu bar, the transport
// strip and the split are SIBLINGS), and walking only the first of them left
// the panes holding the old mode's colours — the bug the screenshot pass
// caught, because the state checks all passed while the screen never changed.
inline void ApplyThemeToWindowViews(BWindow* window) {
    if (window == nullptr) return;
    for (int32 i = 0;; i++) {
        BView* child = window->ChildAt(i);
        if (child == nullptr) break;
        ApplyThemeToView(child);
    }
}

// A window the app did NOT build — a file panel, an alert — in Dark mode. Its
// background is the system panel colour and its lists and text views read the
// system colour table, which the app is no longer allowed to rewrite (T1): so
// the DAW's colours go on explicitly. In System mode there is nothing to do —
// the user's own colours ARE the look.
//
// The window is locked here; call it after Show() (and before an alert's Go(),
// which never returns until the alert is answered).
inline void ThemeStockWindow(BWindow* window);

inline void ApplyThemeToAllWindows() {
    if (be_app == nullptr) return;
    for (int32 i = 0;; i++) {
        BWindow* window = be_app->WindowAt(i);
        if (window == nullptr) break;
        if (!window->Lock())
            continue;
        if (auto* aware = dynamic_cast<ThemeAware*>(window))
            aware->ApplyTheme();
        ApplyThemeToWindowViews(window);
        window->Unlock();
        ThemeStockWindow(window);   // no-op in System mode
    }
}

// --- meter thresholds (ONE set: MeterView and MeterColor disagreed) ------

// -3 dBFS-ish and 0: where a meter turns yellow, and where it turns red.
constexpr float kMeterWarn = 0.70f;
constexpr float kMeterClip = 1.00f;

// A level -> meter color (green below the warn threshold, yellow, red at
// clip). The one place a meter's colours come from. The same in both modes:
// a meter that changed colour with the user's Appearance would be a worse
// meter.
inline rgb_color MeterColor(float level) {
    if (level >= kMeterClip) return Rgb(255, 59, 48);   // red #ff3b30 (at/over 0)
    if (level >= kMeterWarn) return Rgb(255, 204, 0);   // yellow #ffcc00
    return Rgb(52, 199, 89);                            // green #34c759
}

// --- tokens --------------------------------------------------------------
// The names every call site already uses. What they return is the active
// theme's value for that role.

inline rgb_color ColBackground() { return ToRgb(Tokens().background); }
inline rgb_color ColLane()       { return ToRgb(Tokens().lane); }
inline rgb_color ColLaneAlt()    { return ToRgb(Tokens().laneAlt); }
inline rgb_color ColRuler()      { return ToRgb(Tokens().ruler); }
inline rgb_color ColGrid()       { return ToRgb(Tokens().grid); }
inline rgb_color ColClip()       { return ToRgb(Tokens().clip); }
inline rgb_color ColClipBorder() { return ToRgb(Tokens().clipBorder); }
inline rgb_color ColWave()       { return ToRgb(Tokens().wave); }
inline rgb_color ColText()       { return ToRgb(Tokens().text); }
inline rgb_color ColTextDim()    { return ToRgb(Tokens().textDim); }
inline rgb_color ColPlayhead()   { return ToRgb(Tokens().playhead); }
inline rgb_color ColHeader()     { return ToRgb(Tokens().header); }
inline rgb_color ColHeaderHi()   { return ToRgb(Tokens().headerHi); }
inline rgb_color ColAccent()     { return ToRgb(Tokens().accent); }

// Track-type accents.
inline rgb_color ColAudioAccent(){ return ToRgb(Tokens().audioAccent); }
inline rgb_color ColMidiAccent() { return ToRgb(Tokens().midiAccent); }

// Button states (rounded M/S/R/I).
inline rgb_color ColBtnOff()     { return ToRgb(Tokens().btnOff); }
inline rgb_color ColBtnBorder()  { return ToRgb(Tokens().btnBorder); }
inline rgb_color ColBtnText()    { return ToRgb(Tokens().btnText); }
inline rgb_color ColMute()       { return ToRgb(Tokens().mute); }
inline rgb_color ColSolo()       { return ToRgb(Tokens().solo); }
inline rgb_color ColRec()        { return ToRgb(Tokens().rec); }
inline rgb_color ColMon()        { return ToRgb(Tokens().mon); }
inline rgb_color ColPlay()       { return ToRgb(Tokens().play); }

// Knob.
inline rgb_color ColKnobBody()   { return ToRgb(Tokens().knobBody); }
inline rgb_color ColKnobOutline(){ return ToRgb(Tokens().knobOutline); }

// Control-bar / mixer chrome.
inline rgb_color ColChrome()     { return ToRgb(Tokens().chrome); }
inline rgb_color ColChromeHi()   { return ToRgb(Tokens().chromeHi); }
inline rgb_color ColLcd()        { return ToRgb(Tokens().lcd); }
inline rgb_color ColLcdText()    { return ToRgb(Tokens().lcdText); }

// An accent that is an IDENTITY rather than a role — a tempo marker is amber,
// a meter change is blue — but is drawn straight on the panel, so it still has
// to read there: pulled towards the text colour, which darkens it on a light
// panel and brightens it on a dark one.
inline rgb_color PanelAccent(rgb_color c) {
    return MixColor(c, ColText(), 0.35f);
}

// The label colour for a filled area (a clip, a lamp, an accent button):
// whichever of black or white reads better on it. White-on-accent was
// hard-coded before, which breaks the moment the user's accent is pale.
inline rgb_color LabelOn(rgb_color background) {
    return ToRgb(daw::LabelOn(ThemeColor{background.red, background.green,
                                         background.blue}));
}

// Per-track color palette (Track::colorIndex). Index 0 = the default clip
// blue. The same in both modes: a track's colour is the user's, not the
// theme's.
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


// --- menus ---------------------------------------------------------------

// A menu item that draws its label in the theme's text colour.
//
// BMenuItem takes its pen from the SYSTEM colour table (BMenuItem::_HighColor
// reads ui_color(B_MENU_ITEM_TEXT_COLOR)), which the app may no longer rewrite
// (T1) — so on a dark menu the stock item draws black text and the whole menu
// is unreadable. The stock Draw() sets that pen and then calls DrawContent()
// (virtual) followed by the mark, shortcut and submenu symbols, which reuse
// whatever pen is current: setting it here reaches the label AND the shortcut.
//
// A SELECTED item is left alone: its background is the look's (the accent in
// Dark mode, the system's own highlight in System mode) and the system's
// selected-text colour is what belongs on it.
class ThemedMenuItem : public BMenuItem {
public:
    ThemedMenuItem(const char* label, BMessage* message, char shortcut = 0,
                   uint32 modifiers = 0)
        : BMenuItem(label, message, shortcut, modifiers) {}

    // A submenu entry takes its label from the menu's own name (what a menu
    // bar's titles are: the BMenu's name).
    ThemedMenuItem(BMenu* submenu, BMessage* message = nullptr)
        : BMenuItem(submenu, message) {}

    void DrawContent() override {
        if (BMenu* menu = Menu()) {
            if (!(IsSelected() && IsEnabled()))
                menu->SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        }
        BMenuItem::DrawContent();
    }
};

// --- stock windows (a file panel, an alert) -----------------------------

namespace detail {
// One view of a stock window. In Dark mode the wells (lists, text views) take
// the DAW's document colour and everything else its panel colour; in System
// mode the view goes back to the system colour it came with, so a switch back
// is a restore. Buttons and scrollbars draw themselves through the look and
// only need the right panel behind them.
inline void ThemeStockView(BView* view) {
    if (view == nullptr) return;
    const bool well = dynamic_cast<BListView*>(view) != nullptr
                   || dynamic_cast<BTextView*>(view) != nullptr;
    if (ActiveThemeMode() == ThemeMode::Dark) {
        const rgb_color c = well ? ColLcd() : ColHeader();
        view->SetViewColor(c);
        view->SetLowColor(c);
    } else {
        view->SetViewUIColor(well ? B_DOCUMENT_BACKGROUND_COLOR
                                  : B_PANEL_BACKGROUND_COLOR);
        view->SetLowUIColor(well ? B_DOCUMENT_BACKGROUND_COLOR
                                 : B_PANEL_BACKGROUND_COLOR);
    }
    if (well) {
        if (ActiveThemeMode() == ThemeMode::Dark)
            view->SetHighColor(ColText());
        else
            view->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
    }
    for (int32 i = 0; i < view->CountChildren(); i++)
        ThemeStockView(view->ChildAt(i));
    view->Invalidate();
}
} // namespace detail

inline void ThemeStockTopView(BWindow* window) {
    if (window == nullptr) return;
    BLayout* layout = window->GetLayout();
    BView* top = layout != nullptr ? layout->Owner() : nullptr;
    if (top == nullptr) return;
    if (ActiveThemeMode() == ThemeMode::Dark) {
        top->SetViewColor(ColHeader());
        top->SetLowColor(ColHeader());
    } else {
        top->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        top->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
    }
    top->Invalidate();
}

// A stock list (or text view) inside one of the APP's own windows: the well
// the look does not paint for it. Dark mode takes the theme's document colour,
// System mode goes back to the system's own — the same rule ThemeStockView
// applies to stock windows.
inline void ApplyWellColors(BView* well) {
    if (well == nullptr) return;
    if (ActiveThemeMode() == ThemeMode::Dark) {
        well->SetViewColor(ColLcd());
        well->SetLowColor(ColLcd());
        well->SetHighColor(ColText());
    } else {
        well->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        well->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
        well->SetHighUIColor(B_DOCUMENT_TEXT_COLOR);
    }
    well->Invalidate();
}

// --- panel views ---------------------------------------------------------

// A window's root view: it takes the panel colour and re-takes it on a mode
// switch, so a dialog never keeps a stale panel behind the controls it holds.
// Every dialog root in the app is one of these two.
class ThemedView : public BView, public ThemeAware {
public:
    ThemedView(const char* name, uint32 flags)
        : BView(name, flags) {}

    // For a root that fills its window rather than one the layout manages.
    ThemedView(BRect frame, const char* name, uint32 resizingMode, uint32 flags)
        : BView(frame, name, resizingMode, flags) {}

    void ApplyTheme() override {
        SetViewColor(ColHeader());
        SetLowColor(ColHeader());
    }

protected:
    void AttachedToWindow() override {
        BView::AttachedToWindow();
        ApplyTheme();
    }
};

class ThemedGroupView : public BGroupView, public ThemeAware {
public:
    ThemedGroupView(const char* name, orientation posture, float spacing = 0.0f)
        : BGroupView(name, posture, spacing) {}

    void ApplyTheme() override {
        SetViewColor(ColHeader());
        SetLowColor(ColHeader());
    }

protected:
    void AttachedToWindow() override {
        BGroupView::AttachedToWindow();
        ApplyTheme();
    }
};

// The window's own background. Where no child view covers it, what shows is
// the top view the window was born with — the SYSTEM panel colour, which the
// app may not change (T1) — so it is set here like any other view of a stock
// window.
inline void ThemeStockTopView(BWindow* window);

// Every open stock window (a file panel, an alert): the second pass, for a
// window that finished building its views after Show() returned.
inline void ThemeStockWindows();

// The same, for a window that is still being built (an alert before its Go()):
// its views are client-side until Show(), so there is nothing to lock — and
// locking a looper that has not been run is not allowed.
inline void ThemeStockWindowUnlocked(BWindow* window) {
    if (window == nullptr) return;
    if (dynamic_cast<ThemeAware*>(window) != nullptr) return;
    for (int32 i = 0;; i++) {
        BView* child = window->ChildAt(i);
        if (child == nullptr) break;
        detail::ThemeStockView(child);
    }
}

inline void ThemeStockWindow(BWindow* window) {
    if (window == nullptr) return;
    if (!window->Lock())
        return;
    if (dynamic_cast<ThemeAware*>(window) != nullptr) {
        window->Unlock();   // the app's own windows know their own colours
        return;
    }
    ThemeStockTopView(window);
    // The stock colours, over EVERY child (a panel's tree, an alert's layout):
    // ApplyThemeToWindowViews would only re-take the app's own kit colours.
    for (int32 i = 0;; i++) {
        BView* child = window->ChildAt(i);
        if (child == nullptr) break;
        detail::ThemeStockView(child);
    }
    window->Unlock();
}

// The alert subclass below is a BWindow the app DOES own, but it is built by
// the kit: it is themed like a stock one.
inline void ThemeStockWindows() {
    if (be_app == nullptr) return;
    for (int32 i = 0;; i++) {
        BWindow* window = be_app->WindowAt(i);
        if (window == nullptr) break;
        ThemeStockWindow(window);
    }
}

// A stock alert, themed for the mode.
//
// The theming has to happen AFTER Show(): an alert's text view re-adopts the
// system document colours when it is attached to the window, so anything set
// while the alert is being built is undone. Show() is called by Go() on the
// caller's thread -- before Go() starts waiting -- which is the moment the
// window exists and its views are attached.
class ThemedAlertWindow : public BAlert {
public:
    ThemedAlertWindow(const char* title, const char* text, const char* button1,
                      const char* button2 = nullptr, const char* button3 = nullptr,
                      button_width width = B_WIDTH_AS_USUAL,
                      alert_type type = B_INFO_ALERT)
        : BAlert(title, text, button1, button2, button3, width, type) {}

    void Show() override {
        BAlert::Show();
        ThemeStockWindow(this);
        // One more pass a moment later: the kit can still be attaching views
        // (the button row lays out after the first show), and a stale light
        // island in a dark alert is exactly what this is here to prevent.
        BMessageRunner::StartSending(BMessenger(this),
                                     new BMessage(kMsgThemeStock), 200000, 1);
    }

    void MessageReceived(BMessage* message) override {
        if (message->what == kMsgThemeStock) {
            ThemeStockWindow(this);
            return;
        }
        BAlert::MessageReceived(message);
    }

private:
    static constexpr uint32 kMsgThemeStock = 'thst';
};

} // namespace daw
