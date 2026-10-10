// DawControl — the widget kit's shared base (plan M1.3).
//
// The kit's controls are BControls that draw with the theme, so a dialog
// matches the arrangement view instead of falling back to the light system
// look. Every VALUE control gets the same behaviours, and they live here:
// hover / pressed / focus states, double-click to reset, Shift for a fine
// adjustment, the wheel, and a tooltip.
//
// Haiku-only. Geometry is design pixels through Themed() (M1.2), so the kit is
// correct when the user's font size is not 100%.
#pragma once

#include "../Theme.h"

#include <Control.h>
#include <ControlLook.h>
#include <Window.h>

namespace daw {


// A kit control sits on its parent's colour rather than painting its own box
// in the bar colour: the inspector, the dock and the dialogs are not all the
// same shade, and a mismatched tile behind every control is what that looked
// like. A parent that paints itself (transparent) gets the bar colour.
inline void AdoptPanelColors(BView* v) {
    if (BView* p = v->Parent()) {
        const rgb_color pc = p->ViewColor();
        if (pc != B_TRANSPARENT_COLOR) {
            v->SetViewColor(pc);
            v->SetLowColor(pc);
            return;
        }
    }
    const rgb_color c = ColChrome();
    v->SetViewColor(c);
    v->SetLowColor(c);
}

// The colour a control draws itself against, in the control look's terms (its
// `base` argument). Free functions, not members: the kit's stock-derived
// controls (BCheckBox, BSlider, BTextControl, BMenuField) need the same rules
// and are not DawControls.
inline rgb_color PanelColorOf(const BView* view) {
    if (view != nullptr) {
        if (BView* p = const_cast<BView*>(view)->Parent()) {
            const rgb_color pc = p->ViewColor();
            if (pc != B_TRANSPARENT_COLOR) return pc;
        }
    }
    return ColChrome();
}

// Free function, not a member: BMenuField is a BView, not a BControl, and the
// stock and kit controls all need the same state -> flags translation.
inline uint32 LookFlagsOf(bool enabled, bool hover, bool focus, bool pressed) {
    uint32 flags = 0;
    if (!enabled) flags |= BControlLook::B_DISABLED;
    if (hover)    flags |= BControlLook::B_HOVER;
    if (focus)    flags |= BControlLook::B_FOCUSED;
    if (pressed)  flags |= BControlLook::B_CLICKED;
    return flags;
}

class DawControl : public BControl, public ThemeAware {
public:
    // The panel colour this control adopted is cached (a view colour), so a
    // theme change has to re-take it; everything else the kit draws is read
    // from the tokens at draw time.
    void ApplyTheme() override { AdoptPanelColors(this); }

    // Positioning by rectangle, the way today's windows build their controls;
    // the Layout Kit form (below) is what M1.4 uses.
    DawControl(BRect frame, const char* name, const char* label,
               BMessage* message, uint32 resizingMode = B_FOLLOW_LEFT_TOP,
               uint32 flags = B_WILL_DRAW)
        : BControl(frame, name, label, message, resizingMode, flags) {}

    DawControl(const char* name, const char* label, BMessage* message,
               uint32 flags = B_WILL_DRAW)
        : BControl(name, label, message, flags) {}

    void AttachedToWindow() override {
        BControl::AttachedToWindow();
        AdoptPanelColors(this);
        // BButton does this and BControl does not: with no target of its own,
        // a control's message goes to its window. Without it a kit control's
        // Invoke() lands nowhere -- which is exactly how the kit's first test
        // found it.
        if (Target() == nullptr && Window() != nullptr) SetTarget(Window());
        // A control with no tooltip of its own gets its label: the point is
        // that every control carries one.
        if (ToolTip() == nullptr && Label() != nullptr && Label()[0] != '\0')
            SetToolTip(Label());
    }

    // Default size for the Layout Kit (M1.4). Subclasses override.
    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = Themed(80.0f);
        if (height) *height = Themed(22.0f);
    }

    // Hover: a control repaints its own rect on enter/exit. The server sends
    // these transit codes to the view under the cursor; no event mask needed.
    void MouseMoved(BPoint where, uint32 code, const BMessage* drag) override {
        BControl::MouseMoved(where, code, drag);
        const bool hover = (code != B_EXITED_VIEW);
        if (hover != fHover) {
            fHover = hover;
            Invalidate();
        }
    }

    // The wheel over a value control changes it (a no-op for the rest).
    void MessageReceived(BMessage* message) override {
        if (message->what == B_MOUSE_WHEEL_CHANGED) {
            float dy = 0.0f;
            if (message->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f
                && IsEnabled()) {
                WheelChanged(dy);
                return;
            }
        }
        BControl::MessageReceived(message);
    }

protected:
    bool IsHover() const   { return fHover; }
    bool IsPressed() const { return fPressed; }

    // The panel colour this control sits on, and the look's state flags for
    // it — the same rules the free functions above apply, with this control's
    // own hover/press state.
    rgb_color PanelColor() const { return PanelColorOf(this); }
    uint32 LookFlags() const {
        return LookFlagsOf(IsEnabled(), IsHover(), IsFocus(), IsPressed());
    }

    // A stock-looking button: the look's frame, its background, its label
    // placement. In System mode that is the Haiku button; in Dark mode,
    // DawControlLook's.
    void DrawButtonThroughLook(uint32 extraFlags = 0, const char* label = nullptr) {
        BRect r = Bounds();
        const rgb_color base = PanelColor();
        const uint32 flags = LookFlags() | extraFlags;
        be_control_look->DrawButtonFrame(this, r, r, base, base, flags,
                                         BControlLook::B_ALL_BORDERS);
        be_control_look->DrawButtonBackground(this, r, r, base, flags,
                                              BControlLook::B_ALL_BORDERS, B_HORIZONTAL);
        be_control_look->DrawLabel(this, label != nullptr ? label : Label(), r, r,
                                   base, flags, be_control_look->DefaultLabelAlignment(),
                                   nullptr);
    }

    // A value control: double-click returns to this, the wheel/keys move it by
    // one step, Shift is a tenth of a step.
    virtual float DefaultValue() const { return 0.0f; }
    virtual void  ResetToDefault() {}
    virtual void  WheelChanged(float deltaY) { (void)deltaY; }
    virtual bool  ResetsOnDoubleClick() const { return false; }

    // How many "clicks" the current mouse message carries (2 = double-click).
    int32 ClickCount() const {
        int32 clicks = 1;
        if (Window() != nullptr && Window()->CurrentMessage() != nullptr)
            Window()->CurrentMessage()->FindInt32("clicks", &clicks);
        return clicks;
    }

    // Shift held: the caller passes its coarse and fine step.
    float AdjustStep(float coarse, float fine) const {
        return (modifiers() & B_SHIFT_KEY) ? fine : coarse;
    }

    // Press/release visual state for click-style controls.
    void SetPressedVisual(bool pressed) {
        if (pressed == fPressed) return;
        fPressed = pressed;
        Invalidate();
    }

    // A focus ring, drawn inside `r` in the accent colour.
    void DrawFocusRing(BRect r) {
        if (!IsFocus()) return;
        SetHighColor(ColAccent());
        const float w = Themed(1.0f);
        SetPenSize(w);
        StrokeRect(r.InsetBySelf(w, w));
        SetPenSize(1.0f);
    }

private:
    bool fHover   = false;
    bool fPressed = false;
};

} // namespace daw
