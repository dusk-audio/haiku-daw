// DawTextField — the kit's single-line entry (M1.3).
//
// A BTextControl with the theme's colours on both halves: the label draws in
// the control's colours, and the inner BTextView is the recessed LCD well with
// bright text and a visible caret. Everything else (the message, the divider,
// the label) stays stock, so converting a window is swapping the class.
#pragma once

#include "DawControl.h"   // AdoptPanelColors + the look's flag helpers

#include <TextControl.h>
#include <TextView.h>

namespace daw {

class DawTextField : public BTextControl, public ThemeAware {
public:
    // The label half sits on the panel (a cached colour), the text half is the
    // recessed well — both re-taken on a theme change.
    void ApplyTheme() override { TakeThemeColors(); }

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
        TakeThemeColors();
    }

private:
    // The panel behind the label is the parent's, like every other kit control
    // (a fixed lane colour was a light slab in a dark panel the moment the two
    // modes existed). The text half is the recessed well: ColLcd is the
    // document colour in System mode and the dark well in Dark mode, and the
    // caret and selection come from the process colours the same way.
    void TakeThemeColors() {
        AdoptPanelColors(this);
        SetHighColor(ColText());
        if (TextView() != nullptr) {
            TextView()->SetViewColor(ColLcd());
            TextView()->SetLowColor(ColLcd());
            TextView()->SetHighColor(ColText());
            TextView()->SetFont(be_plain_font);
        }
    }

public:

    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = Themed(120.0f);
        if (height) *height = Themed(20.0f);
    }
};

} // namespace daw
