// DawSlider — the kit's linear slider (M1.3).
//
// A BSlider subclass, not a from-scratch control: dragging, the keyboard, the
// hash marks, the focus model and the message protocol all stay stock, and the
// kit only restyles what is drawn. That is what keeps a converted window
// behaving exactly like the one it replaces.
#pragma once

#include "../Theme.h"

#include <Slider.h>

namespace daw {

class DawSlider : public BSlider {
public:
    DawSlider(BRect frame, const char* name, const char* label,
              BMessage* message, int32 minValue, int32 maxValue,
              orientation posture = B_HORIZONTAL,
              thumb_style thumbType = B_BLOCK_THUMB,
              uint32 resizingMode = B_FOLLOW_LEFT_TOP,
              uint32 flags = B_NAVIGABLE | B_WILL_DRAW | B_FRAME_EVENTS)
        : BSlider(frame, name, label, message, minValue, maxValue, posture,
                  thumbType, resizingMode, flags) {}

    DawSlider(const char* name, const char* label, BMessage* message,
              int32 minValue, int32 maxValue,
              orientation posture = B_HORIZONTAL,
              thumb_style thumbType = B_BLOCK_THUMB,
              uint32 flags = B_NAVIGABLE | B_WILL_DRAW | B_FRAME_EVENTS)
        : BSlider(name, label, message, minValue, maxValue, posture, thumbType,
                  flags) {}

    void AttachedToWindow() override {
        BSlider::AttachedToWindow();
        if (Target() == nullptr && Window() != nullptr) SetTarget(Window());
        SetViewColor(ColChrome());
        SetLowColor(ColChrome());
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

protected:
    void DrawBar() override {
        const BRect bar = BarFrame();
        if (!bar.IsValid()) return;
        const float r = Themed(2.0f);
        SetHighColor(ColLcd());                       // the recessed trough
        FillRoundRect(bar, r, r);
        SetHighColor(ColGrid());
        StrokeRoundRect(bar, r, r);

        // The filled part, up to the thumb.
        const BRect thumb = ThumbFrame();
        BRect fill = bar;
        if (Orientation() == B_HORIZONTAL) {
            fill.right = thumb.left + thumb.Width() * 0.5f;
            if (fill.right > bar.right) fill.right = bar.right;
        } else {
            fill.top = thumb.top + thumb.Height() * 0.5f;
            if (fill.top < bar.top) fill.top = bar.top;
        }
        if (fill.IsValid() && fill.Width() >= 0 && fill.Height() >= 0) {
            SetHighColor(ColAccent());
            FillRoundRect(fill, r, r);
        }
    }

    void DrawThumb() override {
        const BRect thumb = ThumbFrame();
        SetHighColor(ColHeaderHi());
        FillRoundRect(thumb, Themed(2.0f), Themed(2.0f));
        SetHighColor(ColBtnBorder());
        StrokeRoundRect(thumb, Themed(2.0f), Themed(2.0f));
        // A light centre line, so the handle reads as a grip.
        SetHighColor(Rgb(232, 232, 238));
        const BPoint c((thumb.left + thumb.right) * 0.5f,
                       (thumb.top + thumb.bottom) * 0.5f);
        if (Orientation() == B_HORIZONTAL)
            StrokeLine(BPoint(c.x, thumb.top + Themed(2.0f)),
                       BPoint(c.x, thumb.bottom - Themed(2.0f)));
        else
            StrokeLine(BPoint(thumb.left + Themed(2.0f), c.y),
                       BPoint(thumb.right - Themed(2.0f), c.y));
    }

    void DrawHashMarks() override {
        if (HashMarkCount() < 2) return;
        const BRect area = HashMarksFrame();
        SetHighColor(ColTextDim());
        const int32 count = HashMarkCount();
        for (int32 i = 0; i < count; i++) {
            const float t = (float)i / (float)(count - 1);
            if (Orientation() == B_HORIZONTAL) {
                const float x = area.left + t * area.Width();
                StrokeLine(BPoint(x, area.top + Themed(2.0f)),
                           BPoint(x, area.bottom - Themed(1.0f)));
            } else {
                const float y = area.bottom - t * area.Height();
                StrokeLine(BPoint(area.left + Themed(2.0f), y),
                           BPoint(area.right - Themed(1.0f), y));
            }
        }
    }

    // The value text BSlider asks for (UpdateText() formats it) in the theme's
    // colours, in the space the bar leaves above it.
    void DrawText() override {
        const char* text = UpdateText();
        if (text == nullptr || text[0] == '\0') return;
        SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        const float tw = StringWidth(text);
        const BRect b = Bounds();
        DrawString(text, BPoint((b.left + b.right) * 0.5f - tw * 0.5f,
                                b.top + Themed(11.0f)));
    }

    void DrawFocusMark() override {
        if (!IsFocus()) return;
        SetHighColor(ColAccent());
        SetPenSize(Themed(1.0f));
        StrokeRect(Bounds().InsetBySelf(Themed(1.0f), Themed(1.0f)));
        SetPenSize(1.0f);
    }
};

} // namespace daw
