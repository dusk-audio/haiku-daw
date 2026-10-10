// DawMenuField — the kit's labelled pop-up field (M1.3).
//
// A BMenuField subclass. The menu itself, the click that opens it, the marked
// item and the field's label all stay stock; what is drawn here is the dark
// frame, the label and the arrow, so the field stops looking like a light
// island on the dark panel (which is what the stock control look would paint).
#pragma once

#include "../Theme.h"

#include <MenuField.h>
#include <MenuItem.h>   // BMenuItem::Label (the marked item)

namespace daw {

class DawMenuField : public BMenuField {
public:
    DawMenuField(BRect frame, const char* name, const char* label, BMenu* menu,
                 uint32 resizingMode = B_FOLLOW_LEFT_TOP,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(frame, name, label, menu, resizingMode, flags) {}

    DawMenuField(const char* name, const char* label, BMenu* menu,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(name, label, menu, flags) {}

    void AttachedToWindow() override {
        BMenuField::AttachedToWindow();
        SetViewColor(ColChrome());
        SetLowColor(ColChrome());
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

    void Draw(BRect) override {
        const BRect b = Bounds();
        const char* label = Label();
        float left = b.left;
        if (label != nullptr && label[0] != '\0') {
            SetHighColor(ColTextDim());
            DrawString(label, BPoint(b.left + Themed(1.0f),
                                     b.top + (b.Height()
                                              + ThemeFontSize() * 0.8f) * 0.5f));
            left += StringWidth(label) + Themed(6.0f);
        }

        // The field itself: a recessed well with the marked item's text.
        BRect well(left, b.top, b.right, b.bottom);
        SetHighColor(ColLcd());
        FillRoundRect(well, Themed(2.0f), Themed(2.0f));
        SetHighColor(ColBtnBorder());
        StrokeRoundRect(well, Themed(2.0f), Themed(2.0f));

        const char* text = "";
        if (Menu() != nullptr) {
            if (BMenuItem* marked = Menu()->FindMarked())
                text = marked->Label();
        }
        SetHighColor(ColText());
        const float baseline = b.top + (b.Height() + ThemeFontSize() * 0.8f) * 0.5f;
        DrawString(text, BPoint(well.left + Themed(5.0f), baseline));

        // The arrow, in the accent colour: a small down triangle at the right.
        const float ax = well.right - Themed(11.0f);
        const float ay = b.top + b.Height() * 0.5f;
        const float s = Themed(3.5f);
        SetHighColor(ColAccent());
        BPoint tri[3] = { BPoint(ax - s, ay - s * 0.6f),
                          BPoint(ax + s, ay - s * 0.6f),
                          BPoint(ax, ay + s * 0.8f) };
        FillPolygon(tri, 3);
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
