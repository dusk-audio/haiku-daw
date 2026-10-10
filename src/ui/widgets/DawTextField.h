// DawTextField — the kit's single-line entry (M1.3).
//
// A BTextControl with the theme's colours on both halves: the label draws in
// the control's colours, and the inner BTextView is the recessed LCD well with
// bright text and a visible caret. Everything else (the message, the divider,
// the label) stays stock, so converting a window is swapping the class.
#pragma once

#include "../Theme.h"

#include <TextControl.h>
#include <TextView.h>

namespace daw {

class DawTextField : public BTextControl {
public:
    DawTextField(BRect frame, const char* name, const char* label,
                 const char* text, BMessage* message,
                 uint32 resizeMask = B_FOLLOW_LEFT_TOP,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BTextControl(frame, name, label, text, message, resizeMask, flags) {}

    DawTextField(const char* name, const char* label, const char* text,
                 BMessage* message,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BTextControl(name, label, text, message, flags) {}

    void AttachedToWindow() override {
        BTextControl::AttachedToWindow();
        // The panel behind the label: the lane/row colour, chosen because it
        // sits close to every panel colour the app uses (chrome, header, lane),
        // so the field blends wherever it is placed.
        SetViewColor(ColLane());
        SetLowColor(ColLane());
        SetHighColor(ColText());
        if (TextView() != nullptr) {
            // High = text, low = the recessed well behind it; the caret and
            // selection come from the system colours, which the app overrides
            // in DawApplication (dark theme), so they stay visible here.
            TextView()->SetViewColor(ColLcd());
            TextView()->SetLowColor(ColLcd());
            TextView()->SetHighColor(ColText());
            TextView()->SetFont(be_plain_font);
        }
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = Themed(120.0f);
        if (height) *height = Themed(20.0f);
    }
};

} // namespace daw
