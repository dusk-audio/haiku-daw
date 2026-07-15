// Small shared custom-drawn widgets (pan knob, vertical fader, button) used by
// both the track-lane header and the inspector. Header-only inline helpers so
// the two views draw identical controls. Kit UI (Haiku-only).
#pragma once

#include "UiMetrics.h"

#include <View.h>

#include <cmath>
#include <cstdio>

namespace daw {

// A rotary pan knob in `r`, pointer angle from pan [-1 (L) .. +1 (R)] (0 = 12
// o'clock). Draws a dial with a tick and an L/C/R hint below.
inline void DrawPanKnob(BView* v, BRect r, float pan) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float rad = std::min(r.Width(), r.Height()) * 0.5f - 1.0f;

    v->SetHighColor(ColHeaderHi());
    v->FillEllipse(BPoint(cx, cy), rad, rad);
    v->SetHighColor(ColGrid());
    v->StrokeEllipse(BPoint(cx, cy), rad, rad);

    // Pointer: pan maps to +/- 140 degrees around 12 o'clock.
    const float ang = (float)(-M_PI / 2.0 + pan * (140.0 * M_PI / 180.0));
    v->SetHighColor(pan == 0.0f ? ColText() : ColAccent());
    v->SetPenSize(2.0f);
    v->StrokeLine(BPoint(cx, cy),
                  BPoint(cx + std::cos(ang) * rad * 0.85f,
                         cy + std::sin(ang) * rad * 0.85f));
    v->SetPenSize(1.0f);
}

// A vertical fader in `r` filled from the bottom to `frac` [0,1]. Draws a unity
// tick at `unityFrac`. Returns nothing; the caller labels it.
inline void DrawVFader(BView* v, BRect r, float frac, float unityFrac) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    v->SetHighColor(ColLcd());
    v->FillRect(r);
    BRect fill = r;
    fill.top = r.bottom - r.Height() * frac;
    v->SetHighColor(ColAccent());
    v->FillRect(fill);
    v->SetHighColor(ColGrid());
    v->StrokeRect(r);
    // Unity tick.
    const float uy = r.bottom - r.Height() * unityFrac;
    v->SetHighColor(ColTextDim());
    v->StrokeLine(BPoint(r.left - 3, uy), BPoint(r.left, uy));
    v->StrokeLine(BPoint(r.right, uy), BPoint(r.right + 3, uy));
}

// A labelled button box; `lit` fills it with an accent/`on` color.
inline void DrawButton(BView* v, BRect r, const char* label, bool lit,
                       rgb_color onColor = ColAccent()) {
    v->SetHighColor(lit ? onColor : ColLane());
    v->FillRect(r);
    v->SetHighColor(ColGrid());
    v->StrokeRect(r);
    v->SetHighColor(lit ? ColBackground() : ColText());
    const float tw = v->StringWidth(label);
    v->DrawString(label, BPoint((r.left + r.right) * 0.5f - tw * 0.5f,
                                r.bottom - 5));
}

// Linear gain <-> fader fraction. Unity (1.0) sits at kUnityFrac so there's
// headroom above; kMaxGainW is the top of the fader travel.
constexpr float kMaxGainW  = 1.5f;    // fader top = +3.5 dB
constexpr float kUnityFrac = 1.0f / kMaxGainW;   // where 1.0 gain sits

inline float GainToFrac(float gain) {
    float f = gain / kMaxGainW;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}
inline float FracToGain(float frac) {
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    return frac * kMaxGainW;
}
inline float GainToDb(float gain) {
    return gain > 1e-4f ? 20.0f * std::log10(gain) : -80.0f;
}

} // namespace daw
