// DawMenuField — the kit's labelled pop-up field (M1.3).
//
// A BMenuField subclass. The menu itself, the click that opens it, the marked
// item and the field's label all stay stock; the field's frame, well and pop-up
// indicator come from the control look (T1), so the field matches the stock
// Haiku one in System mode and the dark panel in Dark mode.
#pragma once

#include "DawControl.h"   // AdoptPanelColors + the look's flag helpers

#include <MenuField.h>
#include <MenuItem.h>   // BMenuItem::Label (the marked item)

namespace daw {

class DawMenuField : public BMenuField, public ThemeAware {
public:
    void ApplyTheme() override { AdoptPanelColors(this); }

    DawMenuField(BRect frame, const char* name, const char* label, BMenu* menu,
                 uint32 resizingMode = B_FOLLOW_LEFT_TOP,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(frame, name, label, menu, resizingMode, flags) {}

    DawMenuField(const char* name, const char* label, BMenu* menu,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(name, label, menu, flags) {}

    void AttachedToWindow() override {
        BMenuField::AttachedToWindow();
        AdoptPanelColors(this);
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

    // The field is drawn through the control look (T1): a stock frame and
    // pop-up indicator in System mode, DawControlLook's recessed well in Dark
    // mode, with the marked item's text and the label placed by the look.
    void Draw(BRect) override {
        const BRect b = Bounds();
        const rgb_color base = PanelColorOf(this);
        const uint32 flags = LookFlagsOf(IsEnabled(), false, IsFocus(), false);
        const char* label = Label();
        float left = b.left;
        if (label != nullptr && label[0] != '\0') {
            // The field's own label ("Output:", "Send to:") in the dim text
            // colour, which is the token that means "a label, not a value".
            SetHighColor(ColTextDim());
            DrawString(label, BPoint(b.left + Themed(1.0f),
                                     b.top + (b.Height()
                                              + ThemeFontSize() * 0.8f) * 0.5f));
            left += StringWidth(label) + Themed(6.0f);
        }

        BRect well(left, b.top, b.right, b.bottom);
        be_control_look->DrawMenuFieldFrame(this, well, b, base, base, flags,
                                            BControlLook::B_ALL_BORDERS);
        be_control_look->DrawMenuFieldBackground(this, well, b, base, true,
                                                 flags);

        const char* text = "";
        if (Menu() != nullptr) {
            if (BMenuItem* marked = Menu()->FindMarked())
                text = marked->Label();
        }
        SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        const float baseline = b.top + (b.Height() + ThemeFontSize() * 0.8f) * 0.5f;
        DrawString(text, BPoint(well.left + Themed(5.0f), baseline));
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width) {
            const char* label = Label();
            *width = (label != nullptr ? StringWidth(label) + Themed(6.0f) : 0.0f)
                   + Themed(110.0f);
        }
        if (height) *height = Themed(20.0f);
    }
};

} // namespace daw
