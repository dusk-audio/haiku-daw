// DawKnob — the kit's rotary control (M1.3).
//
// The dial itself is Widgets.h's shared DrawKnob(), so a kit knob and the
// hand-drawn ones on the timeline and in the effects panel are the same
// drawing. What the control adds is the interaction every value control in the
// kit has: a vertical drag (Shift for fine), the wheel, arrow keys, a
// double-click back to the default, a tooltip, and a message that carries the
// value as a float ("value") as well as BControl's own "be:value".
#pragma once

#include "DawControl.h"
#include "../Widgets.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace daw {

class DawKnob : public DawControl {
public:
    DawKnob(BRect frame, const char* name, const char* label, BMessage* message,
            float minValue, float maxValue, rgb_color accent = ColAccent(),
            uint32 resizingMode = B_FOLLOW_LEFT_TOP,
            uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : DawControl(frame, name, label, message, resizingMode, flags),
          fMin(minValue), fMax(maxValue), fAccent(accent) {
        fFloat = fMin;
        SetValue((int32)std::lround(fFloat));
    }

    DawKnob(const char* name, const char* label, BMessage* message,
            float minValue, float maxValue, rgb_color accent = ColAccent(),
            uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : DawControl(name, label, message, flags),
          fMin(minValue), fMax(maxValue), fAccent(accent) {
        fFloat = fMin;
        SetValue((int32)std::lround(fFloat));
    }

    float FloatValue() const { return fFloat; }

    void SetFloatValue(float v) {
        v = Clamp(v);
        if (v == fFloat) return;
        fFloat = v;
        BControl::SetValue((int32)std::lround(fFloat));
        Invalidate();
    }

    void SetDefaultFloat(float v) { fDefaultFloat = Clamp(v); }
    void SetSuffix(const char* suffix) { fSuffix = suffix; }
    // Bipolar knobs (pan, trim) draw their ring out of the centre.
    void SetBipolar(bool bipolar) { fBipolar = bipolar; Invalidate(); }

    status_t Invoke(BMessage* message = nullptr) override {
        BMessage* msg = message != nullptr ? message : Message();
        if (msg != nullptr) msg->AddFloat("value", fFloat);
        return BControl::Invoke(message);
    }

    void Draw(BRect) override {
        const BRect b = Bounds();
        const float lineH = ThemeFontSize() + Themed(2.0f);
        // Room for exactly the text lines drawn under the dial (the value, and
        // the label when there is one); reserving less clipped the label.
        const bool hasLabel = Label() != nullptr && Label()[0] != '\0';
        const float textH = lineH * (hasLabel ? 2.0f : 1.0f) + Themed(2.0f);
        const float dialH = std::max(b.Height() - textH, Themed(12.0f));
        const float d = std::min(b.Width(), dialH);
        BRect dial(b.left + (b.Width() - d) * 0.5f, b.top, 0, b.top + d);
        dial.right = dial.left + d;

        DrawKnob(this, dial, DialValue(), fAccent, fBipolar);

        SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        char text[32];
        std::snprintf(text, sizeof(text),
                      (fMax - fMin) <= 4.0f ? "%.2f%s" : "%.0f%s",
                      fFloat, fSuffix);
        const float tw = StringWidth(text);
        DrawString(text, BPoint((b.left + b.right) * 0.5f - tw * 0.5f,
                                dial.bottom + ThemeFontSize()));
        const float labelY = dial.bottom + ThemeFontSize() + lineH;
        if (hasLabel && labelY <= b.bottom) {
            SetHighColor(ColTextDim());
            const float lw = StringWidth(Label());
            DrawString(Label(), BPoint((b.left + b.right) * 0.5f - lw * 0.5f,
                                       labelY));
        }
        DrawFocusRing(b);
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width)  *width  = Themed(46.0f);
        if (height) *height = Themed(62.0f);
    }

protected:
    void MouseDown(BPoint where) override {
        if (!IsEnabled()) return;
        if (ClickCount() > 1) {          // back to the default
            SetFloatValue(fDefaultFloat);
            Invoke();
            return;
        }
        fDragging = true;
        fGrabY = where.y;
        fGrabValue = fFloat;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override {
        if (fDragging) {
            // Vertical drag: a full travel over ~140 design pixels. Shift is
            // the fine rate, like every other control in the kit.
            const float travel = Themed(AdjustStep(140.0f, 700.0f));
            const float delta = (fGrabY - where.y) / travel * (fMax - fMin);
            SetFloatValue(fGrabValue + delta);
            return;
        }
        DawControl::MouseMoved(where, transit, drag);
    }

    void MouseUp(BPoint) override {
        if (!fDragging) return;
        fDragging = false;
        Invoke();
    }

    void KeyDown(const char* bytes, int32 numBytes) override {
        if (numBytes < 1) { DawControl::KeyDown(bytes, numBytes); return; }
        const float step = AdjustStep((fMax - fMin) / 100.0f,
                                      (fMax - fMin) / 1000.0f);
        switch (bytes[0]) {
            case B_UP_ARROW:   SetFloatValue(fFloat + step); Invoke(); return;
            case B_DOWN_ARROW: SetFloatValue(fFloat - step); Invoke(); return;
            case B_HOME:       SetFloatValue(fMin); Invoke(); return;
            case B_END:        SetFloatValue(fMax); Invoke(); return;
            default: break;
        }
        DawControl::KeyDown(bytes, numBytes);
    }

    void WheelChanged(float deltaY) override {
        const float step = AdjustStep((fMax - fMin) / 50.0f,
                                      (fMax - fMin) / 500.0f);
        SetFloatValue(fFloat + deltaY * step);
        Invoke();
    }

    void ResetToDefault() override { SetFloatValue(fDefaultFloat); }
    bool  ResetsOnDoubleClick() const override { return true; }
    float DefaultValue() const override { return fDefaultFloat; }

private:
    float Clamp(float v) const {
        if (v < fMin) return fMin;
        if (v > fMax) return fMax;
        return v;
    }
    // The dial's -1..1 value for the drawn range.
    float DialValue() const {
        if (fMax <= fMin) return 0.0f;
        return 2.0f * ((fFloat - fMin) / (fMax - fMin)) - 1.0f;
    }

    float fMin;
    float fMax;
    float fFloat      = 0.0f;
    float fDefaultFloat = 0.0f;   // the minimum until a default is set
    float fGrabY      = 0.0f;
    float fGrabValue  = 0.0f;
    bool  fDragging   = false;
    bool  fBipolar    = false;
    rgb_color fAccent;
    const char* fSuffix = "";
};

} // namespace daw
