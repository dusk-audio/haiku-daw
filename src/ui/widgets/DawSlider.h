// DawSlider — the kit's linear slider (M1.3).
//
// A BSlider subclass, not a from-scratch control: dragging, the keyboard, the
// hash marks, the focus model and the message protocol all stay stock, and the
// kit only restyles what is drawn. That is what keeps a converted window
// behaving exactly like the one it replaces.
#pragma once

#include "DawControl.h"   // AdoptPanelColors

#include "../Theme.h"

#include <Slider.h>
#include <Window.h>

#include <cmath>

namespace daw {

class DawSlider : public BSlider, public ThemeAware {
public:
    // The trough and the thumb come from the control look, the filled part
    // from the theme accent, so a slider reads as a slider in both modes.
    void ApplyTheme() override { AdoptPanelColors(this); }

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
        AdoptPanelColors(this);
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

    // The value a double-click returns to (the minimum until one is set).
    void SetDefaultValue(int32 value) { fDefaultValue = value; }

    // The drag model: clicking the trough jumps there (the stock gesture),
    // then dragging is RELATIVE to the grab point, which is what makes Shift
    // for a fine rate possible at all. The keyboard, the hash marks and the
    // focus model stay the base class's.
    void MouseDown(BPoint where) override {
        if (!IsEnabled()) return;
        MakeFocus(true);
        int32 clicks = 1;
        if (Window() != nullptr && Window()->CurrentMessage() != nullptr)
            Window()->CurrentMessage()->FindInt32("clicks", &clicks);
        if (clicks > 1) {
            BSlider::SetValue(fDefaultValue);
            Invoke();
            return;
        }
        if (!ThumbFrame().Contains(where)) {
            const int32 jumped = ValueForPoint(where);
            if (jumped != Value()) {
                BSlider::SetValue(jumped);
                Invoke();
            }
        }
        fDragging  = true;
        fGrabPoint = where;
        fGrabValue = Value();
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override {
        if (!fDragging) {
            BSlider::MouseMoved(where, transit, drag);
            return;
        }
        const BRect bar = BarFrame();
        if (bar.Width() <= 0.0f || bar.Height() <= 0.0f) return;
        const int32 vMin = ValueForPoint(BPoint(bar.left, bar.top));
        const int32 vMax = ValueForPoint(BPoint(bar.right, bar.bottom));
        const bool horizontal = Orientation() == B_HORIZONTAL;
        const float span = horizontal ? bar.Width() : bar.Height();
        float d = horizontal ? (where.x - fGrabPoint.x) / span
                             : (fGrabPoint.y - where.y) / span;
        if ((modifiers() & B_SHIFT_KEY) != 0) d /= 10.0f;
        int32 v = fGrabValue + (int32)std::lround(d * (vMax - vMin));
        if (v < vMin) v = vMin;
        if (v > vMax) v = vMax;
        if (v != Value()) {
            BSlider::SetValue(v);
            if (ModificationMessage() != nullptr)
                Invoke(ModificationMessage());
        }
    }

    void MouseUp(BPoint where) override {
        if (!fDragging) { BSlider::MouseUp(where); return; }
        fDragging = false;
        Invoke();
    }

protected:
    void DrawBar() override {
        const BRect bar = BarFrame();
        if (!bar.IsValid()) return;
        const float r = Themed(2.0f);

        // The recessed trough is the look's (stock in System mode, the dark
        // well in Dark mode) ...
        be_control_look->DrawSliderBar(this, bar, Bounds(), PanelColorOf(this),
                                       ColLcd(),
                                       LookFlagsOf(IsEnabled(), false, IsFocus(),
                                                   false),
                                       Orientation());
        // ... and the filled part is the theme accent, in both modes: which
        // end of the range the handle is at is the slider's whole job.

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
        BRect thumb = ThumbFrame();
        be_control_look->DrawSliderThumb(this, thumb, Bounds(), PanelColorOf(this),
                                         LookFlagsOf(IsEnabled(), false, IsFocus(),
                                                     fDragging),
                                         Orientation());
        // A centre line, so the handle reads as a grip whatever the look drew.
        SetHighColor(ColText());
        const BPoint c((thumb.left + thumb.right) * 0.5f,
                       (thumb.top + thumb.bottom) * 0.5f);
        if (Orientation() == B_HORIZONTAL)
            StrokeLine(BPoint(c.x, thumb.top + Themed(3.0f)),
                       BPoint(c.x, thumb.bottom - Themed(3.0f)));
        else
            StrokeLine(BPoint(thumb.left + Themed(3.0f), c.y),
                       BPoint(thumb.right - Themed(3.0f), c.y));
    }

    void DrawHashMarks() override {
        if (HashMarkCount() < 2) return;
        BRect area = HashMarksFrame();
        be_control_look->DrawSliderHashMarks(this, area, Bounds(),
                                             PanelColorOf(this), HashMarkCount(),
                                             B_HASH_MARKS_BOTTOM,
                                             LookFlagsOf(IsEnabled(), false,
                                                         IsFocus(), false),
                                             Orientation());
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

    int32 fDefaultValue = 0;
    bool  fDragging     = false;
    BPoint fGrabPoint;
    int32 fGrabValue    = 0;

    void DrawFocusMark() override {
        if (!IsFocus()) return;
        SetHighColor(ColAccent());
        SetPenSize(Themed(1.0f));
        StrokeRect(Bounds().InsetBySelf(Themed(1.0f), Themed(1.0f)));
        SetPenSize(1.0f);
    }
};

} // namespace daw
