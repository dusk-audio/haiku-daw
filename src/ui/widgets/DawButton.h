// DawButton / DawToggle — the kit's push button and latching toggle (M1.3).
//
// Both draw with Widgets.h's shared DrawButton(), so a kit button and a
// hand-drawn one on the timeline look identical. Hover is a light wash,
// pressed a dark one, and the focus ring is the theme accent. A DawToggle
// keeps a value (B_CONTROL_ON/OFF) and shows it lit in its colour — the
// M/S/R/I lamps are the same drawing with a different colour.
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

    void Draw(BRect) override {
        DrawButton(this, Bounds(), Label(), Value() == B_CONTROL_ON, fOnColor);
        if (fDefault && Value() != B_CONTROL_ON) {   // default: accent border
            SetHighColor(ColAccent());
            SetPenSize(Themed(1.0f));
            StrokeRoundRect(Bounds().InsetBySelf(0.5f, 0.5f),
                            Themed(3.0f), Themed(3.0f));
            SetPenSize(1.0f);
        }
        // States the shared drawing has no opinion about: a light wash under
        // the cursor, a dark one while the button is held down.
        if (IsPressed() || IsHover()) {
            SetDrawingMode(B_OP_ALPHA);
            if (IsPressed()) SetHighColor(0, 0, 0, 70);
            else             SetHighColor(255, 255, 255, 26);
            FillRoundRect(Bounds(), Themed(3.0f), Themed(3.0f));
            SetDrawingMode(B_OP_COPY);
        }
        DrawFocusRing(Bounds());
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
