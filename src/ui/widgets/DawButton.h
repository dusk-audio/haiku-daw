// DawButton / DawToggle — the kit's push button and latching toggle (M1.3).
//
// Both draw through the control look (T1), so the button matches the stock
// Haiku one in System mode and DawControlLook's in Dark mode; hover, pressed,
// focus and disabled come from the look's own flags. A DawToggle keeps a value
// (B_CONTROL_ON/OFF) and shows it lit in its colour — the M/S/R/I lamps are a
// colour the look cannot express, so a lit toggle is drawn by the control with
// an auto-contrast label.
//
// The timeline's hand-drawn boxes still use Widgets.h's DrawButton(): they are
// painted inside the lane, on the lane's own background, not laid out as
// controls.
#pragma once

#include "DawControl.h"
#include "../Widgets.h"

namespace daw {

class DawButton : public DawControl {
public:
    DawButton(BRect frame, const char* name, const char* label,
              BMessage* message, rgb_color onColor = ColAccent(),
              uint32 resizingMode = B_FOLLOW_LEFT_TOP,
              uint32 flags = B_WILL_DRAW)
        : DawControl(frame, name, label, message, resizingMode, flags),
          fOnColor(onColor) {}

    DawButton(const char* name, const char* label, BMessage* message,
              rgb_color onColor = ColAccent())
        : DawControl(name, label, message), fOnColor(onColor) {}

    // BControl has no notion of a default button (that is BButton's); the
    // kit's is a drawn accent border, which is all a default button has to be.
    void MakeDefault(bool on = true) {
        fDefault = on;
        Invalidate();
    }

    // The control look paints the button (T1): in System mode that is the
    // stock Haiku button, in Dark mode DawControlLook's — one call, so a kit
    // button and a stock one can never drift apart.
    //
    // The one state the look cannot express is a LIT toggle: "this button
    // means REC" is a colour, not a flag. So an on toggle is drawn here in its
    // own colour with an auto-contrast label, exactly as the timeline's M/S/R
    // lamps are.
    void Draw(BRect) override {
        const bool on = Value() == B_CONTROL_ON;
        const rgb_color base = PanelColor();
        if (on) {
            SetHighColor(fOnColor);
            FillRoundRect(Bounds(), Themed(3.0f), Themed(3.0f));
            SetHighColor(LabelOn(fOnColor));
            const float tw = StringWidth(Label());
            DrawString(Label(), BPoint(Bounds().left
                                           + (Bounds().Width() - tw) * 0.5f,
                                       Bounds().bottom - Themed(6.0f)));
        } else {
            DrawButtonThroughLook(fDefault ? BControlLook::B_DEFAULT_BUTTON : 0);
        }
    }

    void MouseDown(BPoint where) override {
        if (!IsEnabled()) { BControl::MouseDown(where); return; }
        SetPressedVisual(true);
        BControl::MouseDown(where);
    }

    void MouseUp(BPoint where) override {
        const bool completed = IsPressed() && IsEnabled()
                            && Bounds().Contains(where);
        SetPressedVisual(false);
        if (completed) Invoke();
        BControl::MouseUp(where);
    }

    // Space presses the focused button, like the stock one.
    void KeyDown(const char* bytes, int32 numBytes) override {
        if (numBytes == 1 && bytes[0] == B_SPACE) {
            Invoke();
            return;
        }
        DawControl::KeyDown(bytes, numBytes);
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = StringWidth(Label()) + Themed(24.0f);
        if (height) *height = Themed(22.0f);
    }

private:
    rgb_color fOnColor;
    bool      fDefault = false;
};

// A latching toggle: a click flips B_CONTROL_OFF <-> B_CONTROL_ON, then the
// usual press completes and invokes — with the new value already in place, the
// way a stock BCheckBox reports.
class DawToggle : public DawButton {
public:
    DawToggle(BRect frame, const char* name, const char* label,
              BMessage* message, rgb_color onColor = ColAccent(),
              uint32 resizingMode = B_FOLLOW_LEFT_TOP,
              uint32 flags = B_WILL_DRAW)
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
