// DawCheckBox — the kit's tick box (M1.3).
//
// A BCheckBox subclass: the click, the value flip, the keyboard and the
// B_CONTROL_ON/OFF message all stay stock, and the box and the check mark are
// drawn with the theme (the stock look would paint a light box on the dark
// panel).
#pragma once

#include "DawControl.h"   // AdoptPanelColors

#include "../Theme.h"

#include <CheckBox.h>

namespace daw {

class DawCheckBox : public BCheckBox {
public:
    DawCheckBox(BRect frame, const char* name, const char* label,
                BMessage* message,
                uint32 resizingMode = B_FOLLOW_LEFT_TOP,
                uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BCheckBox(frame, name, label, message, resizingMode, flags) {}

    DawCheckBox(const char* name, const char* label, BMessage* message,
                uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BCheckBox(name, label, message, flags) {}

    void AttachedToWindow() override {
        BCheckBox::AttachedToWindow();
        if (Target() == nullptr && Window() != nullptr) SetTarget(Window());
        AdoptPanelColors(this);
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

    void Draw(BRect) override {
        const BRect b = Bounds();
        const float box = Themed(13.0f);
        // The box sits on the first text line, whatever the control's height.
        const float top = b.top + (b.Height() - box) * 0.5f;
        BRect r(b.left + Themed(1.0f), top, b.left + Themed(1.0f) + box,
                top + box);

        const bool on = Value() == B_CONTROL_ON;
        SetHighColor(on ? ColAccent() : ColLcd());
        FillRoundRect(r, Themed(2.0f), Themed(2.0f));
        SetHighColor(on ? ColAccent() : ColBtnBorder());
        StrokeRoundRect(r, Themed(2.0f), Themed(2.0f));

        if (on) {   // the tick: two strokes of a check mark
            SetHighColor(Rgb(255, 255, 255));
            SetPenSize(Themed(1.5f));
            const BPoint a(r.left + box * 0.24f, r.top + box * 0.52f);
            const BPoint m(r.left + box * 0.44f, r.top + box * 0.72f);
            const BPoint e(r.left + box * 0.78f, r.top + box * 0.30f);
            StrokeLine(a, m);
            StrokeLine(m, e);
            SetPenSize(1.0f);
        }

        SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        // Baseline: the box's centre, nudged down by the font's own half-height.
        DrawString(Label() != nullptr ? Label() : "",
                   BPoint(r.right + Themed(6.0f),
                          r.top + box * 0.5f + ThemeFontSize() * 0.4f));

        if (IsFocus()) {
            SetHighColor(ColAccent());
            SetPenSize(Themed(1.0f));
            StrokeRect(b.InsetByCopy(Themed(1.0f), Themed(1.0f)));
            SetPenSize(1.0f);
        }
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width)
            *width = Themed(14.0f) + Themed(6.0f)
                   + StringWidth(Label() != nullptr ? Label() : "")
                   + Themed(4.0f);
        if (height) *height = Themed(18.0f);
    }
};

} // namespace daw
