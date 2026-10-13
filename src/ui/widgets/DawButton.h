// DawButton / DawToggle — the kit's push button and latching toggle (M1.3).
//
// A BButton subclass, not a bare BControl one: BButton is the class that
// carries the DEFAULT button. BWindow::SetDefaultButton takes a BButton* and
// calls the virtual BButton::MakeDefault on the pointer it stored, and
// BWindow::_DetermineTarget routes Return to that button before the focus view
// (Haiku sources: src/kits/interface/Window.cpp:1813, :3284; Button.cpp:242).
// Registering anything that is not really a BButton through that API would walk
// the wrong vtable, so the kit's button IS one — MakeDefault() below can hand
// itself to the window with no cast.
//
// The drawing stays the kit's: both draw through the control look (T1), so the
// button matches the stock Haiku one in System mode and DawControlLook's in
// Dark mode; hover, pressed, focus and disabled come from the look's own flags,
// and a default button asks the look for B_DEFAULT_BUTTON. A DawToggle keeps a
// value (B_CONTROL_ON/OFF) and shows it lit in its colour — the M/S/R/I lamps
// are a colour the look cannot express, so a lit toggle is drawn by the control
// with an auto-contrast label.
//
// The timeline's hand-drawn boxes still use Widgets.h's DrawButton(): they are
// painted inside the lane, on the lane's own background, not laid out as
// controls.
#pragma once

#include "DawControl.h"   // AdoptPanelColors, PanelColorOf, LookFlagsOf
#include "../Theme.h"
#include "../Widgets.h"

#include <Button.h>
#include <Window.h>

namespace daw {

class DawButton : public BButton, public ThemeAware {
public:
    // A kit control sits on its parent's colour; the adopted view colour is
    // cached, so a theme switch re-takes it.
    void ApplyTheme() override { AdoptPanelColors(this); }

    DawButton(BRect frame, const char* name, const char* label,
              BMessage* message, rgb_color onColor = ColAccent(),
              uint32 resizingMode = B_FOLLOW_LEFT_TOP,
              uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BButton(frame, name, label, message, resizingMode, flags),
          fOnColor(onColor) {}

    DawButton(const char* name, const char* label, BMessage* message,
              rgb_color onColor = ColAccent())
        : BButton(name, label, message, B_WILL_DRAW | B_NAVIGABLE),
          fOnColor(onColor) {}

    void AttachedToWindow() override {
        BButton::AttachedToWindow();
        AdoptPanelColors(this);
        // BButton does this and BControl does not: with no target of its own, a
        // control's message goes to its window. Without it a kit control's
        // Invoke() lands nowhere -- which is exactly how the kit's first test
        // found it.
        if (Target() == nullptr && Window() != nullptr) SetTarget(Window());
        if (ToolTip() == nullptr && Label() != nullptr && Label()[0] != '\0')
            SetToolTip(Label());
        // A dialog makes its primary button the default while it BUILDS, before
        // the button has a window (MakeDefault(true) in the constructor); the
        // window-level registration can only happen now.
        if (fDefault) MakeDefault(true);
    }

    // The default button: the accent border this draws AND the registration
    // that makes Return accept the dialog. BWindow::SetDefaultButton is where
    // Return is routed to, and it calls this virtual back to tell the button so;
    // the window's record is this button already at that point, so the call
    // cannot recurse.
    //
    // `false` deliberately only clears the button's own state: BWindow calls
    // MakeDefault(false) from INSIDE SetDefaultButton (leaving one default for
    // another), where calling SetDefaultButton again would loop.
    void MakeDefault(bool on = true) override {
        fDefault = on;
        if (on) {
            if (BWindow* w = Window())
                if (w->DefaultButton() != this) w->SetDefaultButton(this);
        }
        Invalidate();
    }

    // The control look paints the button (T1): in System mode that is the stock
    // Haiku button, in Dark mode DawControlLook's — one call, so a kit button
    // and a stock one can never drift apart.
    //
    // The one state the look cannot express is a LIT toggle: "this button means
    // REC" is a colour, not a flag. So an on toggle is drawn here in its own
    // colour with an auto-contrast label, exactly as the timeline's M/S/R lamps
    // are.
    void Draw(BRect) override {
        const bool on = Value() == B_CONTROL_ON;
        const rgb_color base = PanelColorOf(this);
        if (on) {
            SetHighColor(fOnColor);
            FillRoundRect(Bounds(), Themed(3.0f), Themed(3.0f));
            SetHighColor(LabelOn(fOnColor));
            const float tw = StringWidth(Label());
            DrawString(Label(), BPoint(Bounds().left
                                           + (Bounds().Width() - tw) * 0.5f,
                                       Bounds().bottom - Themed(6.0f)));
        } else {
            BRect r = Bounds();
            const uint32 flags = LookFlagsOf(IsEnabled(), fHover, IsFocus(),
                                             fPressed)
                               | (fDefault ? BControlLook::B_DEFAULT_BUTTON : 0);
            be_control_look->DrawButtonFrame(this, r, r, base, base, flags,
                                             BControlLook::B_ALL_BORDERS);
            be_control_look->DrawButtonBackground(this, r, r, base, flags,
                                                  BControlLook::B_ALL_BORDERS,
                                                  B_HORIZONTAL);
            be_control_look->DrawLabel(this, Label() != nullptr ? Label() : "",
                                       r, r, base, flags,
                                       be_control_look->DefaultLabelAlignment(),
                                       nullptr);
        }
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

    void MouseDown(BPoint where) override {
        if (!IsEnabled()) { BControl::MouseDown(where); return; }
        SetPressedVisual(true);
        BControl::MouseDown(where);
    }

    void MouseUp(BPoint where) override {
        const bool completed = fPressed && IsEnabled()
                            && Bounds().Contains(where);
        SetPressedVisual(false);
        if (completed) Invoke();
        BControl::MouseUp(where);
    }

    // BControl's own invoke, deliberately NOT BButton's. BButton::Invoke ends
    // with SetValue(B_CONTROL_OFF) for a button whose behaviour is not
    // B_TOGGLE_BEHAVIOR, which unlatches every DawToggle in the app the moment
    // it is clicked -- the M/S/R/I lamps, the mixer's, the dialog toggles --
    // and makes the second click report ON again. (It also Sync()s the window
    // and sleeps 50 ms per click, neither of which the kit's buttons want.)
    // The value a control reports is the control's, not the base class's.
    status_t Invoke(BMessage* message = nullptr) override {
        return BControl::Invoke(message);
    }

    // Space presses the focused button and Return presses the DEFAULT one --
    // BWindow routes the key to this view for that, which is the whole point of
    // MakeDefault. Both invoke; no base call is needed for them (BButton's
    // version adds a 25 ms flash and dereferences Window(), which a view that
    // is being torn down may no longer have).
    void KeyDown(const char* bytes, int32 numBytes) override {
        if (numBytes == 1 && (bytes[0] == B_SPACE || bytes[0] == B_ENTER)) {
            Invoke();
            return;
        }
        BControl::KeyDown(bytes, numBytes);
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = StringWidth(Label()) + Themed(24.0f);
        if (height) *height = Themed(22.0f);
    }

protected:
    bool IsPressed() const { return fPressed; }

    void SetPressedVisual(bool pressed) {
        if (pressed == fPressed) return;
        fPressed = pressed;
        Invalidate();
    }

private:
    rgb_color fOnColor;
    bool      fDefault = false;
    bool      fHover   = false;
    bool      fPressed = false;
};

// A latching toggle: a click flips B_CONTROL_OFF <-> B_CONTROL_ON, then the
// usual press completes and invokes — with the new value already in place, the
// way a stock BCheckBox reports.
class DawToggle : public DawButton {
public:
    DawToggle(BRect frame, const char* name, const char* label,
              BMessage* message, rgb_color onColor = ColAccent(),
              uint32 resizingMode = B_FOLLOW_LEFT_TOP,
              uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : DawButton(frame, name, label, message, onColor, resizingMode, flags) {}

    DawToggle(const char* name, const char* label, BMessage* message,
              rgb_color onColor = ColAccent())
        : DawButton(name, label, message, onColor) {}

    void MouseUp(BPoint where) override {
        if (IsPressed() && IsEnabled() && Bounds().Contains(where))
            SetValue(Value() == B_CONTROL_ON ? B_CONTROL_OFF : B_CONTROL_ON);
        DawButton::MouseUp(where);
    }
};

} // namespace daw
