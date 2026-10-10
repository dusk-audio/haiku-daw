// DawCheckBox — the kit's tick box (M1.3).
//
// A BCheckBox subclass: the click, the value flip, the keyboard and the
// B_CONTROL_ON/OFF message all stay stock, and the box and its label are drawn
// through the control look (T1) — the stock Haiku box in System mode,
// DawControlLook's in Dark mode.
#pragma once

#include "DawControl.h"   // AdoptPanelColors

#include "../Theme.h"

#include <CheckBox.h>

namespace daw {

class DawCheckBox : public BCheckBox, public ThemeAware {
public:
    void ApplyTheme() override { AdoptPanelColors(this); }

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

        const rgb_color base = PanelColorOf(this);
        uint32 flags = LookFlagsOf(IsEnabled(), false, IsFocus(), false);
        if (Value() == B_CONTROL_ON) flags |= BControlLook::B_ACTIVATED;
        be_control_look->DrawCheckBox(this, r, b, base, flags);

        // The label, through the look: it carries the disabled colouring and
        // the vertical centring, so the box and the text share one baseline
        // rule in both modes.
        const BRect labelRect(r.right + Themed(6.0f), b.top, b.right, b.bottom);
        be_control_look->DrawLabel(this, Label() != nullptr ? Label() : "",
                                   labelRect, b, base, flags,
                                   BAlignment(B_ALIGN_LEFT,
                                              B_ALIGN_VERTICAL_CENTER),
                                   nullptr);

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
