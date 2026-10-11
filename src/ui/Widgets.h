// Small shared custom-drawn widgets (pan knob, fader, VU meter, button) used by
// both the track-lane header and the inspector, in the Logic-Slate dark theme.
// Header-only inline helpers so the two views draw identical controls.
// Haiku-only (BeAPI drawing).
#pragma once

#include "UiMetrics.h"

#include <View.h>
#include <GradientLinear.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

// A rotary knob: dark body + outline, a colored value ring (FillArc-style, drawn
// as a thick stroked arc) from 12 o'clock to the value, and a crisp white
// pointer. `val` in [-1,1] (bipolar, e.g. pan; 0 = 12 o'clock). `accent` colors
// the value ring. The knob sweeps +/- 140 degrees from top.
// `fromCentre`: a bipolar knob (pan, gain trim) lights its ring from 12
// o'clock; a unipolar one (a 0..100% amount) lights it from the minimum end.
inline void DrawKnob(BView* v, BRect r, float val, rgb_color accent,
                     bool fromCentre = true) {
    if (!(val >= -1.0f)) val = -1.0f;   // NaN/low -> -1
    if (val > 1.0f) val = 1.0f;
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    const float rad = std::min(r.Width(), r.Height()) * 0.5f - Themed(3.0f);
    if (rad < Themed(2.0f)) return;
    const float ringR = rad + Themed(2.0f);

    // Value-ring track (dim full sweep) then the lit value portion. BeAPI arc
    // angles: degrees CCW, 0 = 3 o'clock, 90 = 12 o'clock (top).
    v->SetPenSize(Themed(2.5f));
    v->SetHighColor(ColGrid());
    v->StrokeArc(BPoint(cx, cy), ringR, ringR, -50.0f, 280.0f);   // full track
    v->SetHighColor(accent);
    if (fromCentre)
        v->StrokeArc(BPoint(cx, cy), ringR, ringR, 90.0f, -val * 140.0f);
    else
        v->StrokeArc(BPoint(cx, cy), ringR, ringR, -50.0f, (val + 1.0f) * 140.0f);
    v->SetPenSize(1.0f);

    // Body.
    v->SetHighColor(ColKnobBody());
    v->FillEllipse(BPoint(cx, cy), rad, rad);
    v->SetHighColor(ColKnobOutline());
    v->StrokeEllipse(BPoint(cx, cy), rad, rad);

    // Pointer (standard math angle; screen y is down, so subtract sin).
    const double ang = (90.0 - val * 140.0) * M_PI / 180.0;
    v->SetHighColor(Rgb(235, 235, 240));
    v->SetPenSize(Themed(2.0f));
    v->StrokeLine(BPoint(cx, cy),
                  BPoint(cx + std::cos(ang) * rad * 0.78f,
                         cy - std::sin(ang) * rad * 0.78f));
    v->SetPenSize(1.0f);
}

inline void DrawPanKnob(BView* v, BRect r, float pan) {
    DrawKnob(v, r, pan, ColAccent());
}

// A rounded button. Inactive: dark fill + subtle border + dim text. Active:
// `onColor` fill with auto-contrast (black on bright, white on dark) text.
inline void DrawButton(BView* v, BRect r, const char* label, bool active,
                       rgb_color onColor = ColAccent()) {
    if (active) {
        v->SetHighColor(onColor);
        v->FillRoundRect(r, Themed(3.0f), Themed(3.0f));
    } else {
        v->SetHighColor(ColBtnOff());
        v->FillRoundRect(r, Themed(3.0f), Themed(3.0f));
        v->SetHighColor(ColBtnBorder());
        v->StrokeRoundRect(r, Themed(3.0f), Themed(3.0f));
    }
    rgb_color txt = ColBtnText();
    if (active) {
        const float lum = 0.299f * onColor.red + 0.587f * onColor.green
                        + 0.114f * onColor.blue;
        txt = lum > 140.0f ? Rgb(20, 20, 22) : Rgb(240, 240, 240);
    }
    v->SetHighColor(txt);
    const float tw = v->StringWidth(label);
    v->DrawString(label, BPoint((r.left + r.right) * 0.5f - tw * 0.5f,
                                r.bottom - Themed(6.0f)));
}

// A vertical channel fader in `r`: a narrow dark trough with a unity tick and a
// metallic rectangular cap (24x12) carrying a white indicator line. `frac` is
// the fader position [0,1]; `unityFrac` marks 0 dB.
inline void DrawFader(BView* v, BRect r, float frac, float unityFrac) {
    if (!(frac > 0)) frac = 0; if (frac > 1) frac = 1;
    const float cx = (r.left + r.right) * 0.5f;

    // Trough (recessed near-black).
    BRect trough(cx - Themed(2.0f), r.top, cx + Themed(2.0f), r.bottom);
    v->SetHighColor(ColGrid());
    v->FillRect(trough);
    v->SetHighColor(Rgb(10, 10, 13));
    v->StrokeLine(BPoint(trough.left, r.top), BPoint(trough.left, r.bottom));

    // Unity (0 dB) tick.
    const float uy = r.bottom - r.Height() * unityFrac;
    v->SetHighColor(ColTextDim());
    v->StrokeLine(BPoint(cx - Themed(8.0f), uy), BPoint(cx - Themed(4.0f), uy));
    v->StrokeLine(BPoint(cx + Themed(4.0f), uy), BPoint(cx + Themed(8.0f), uy));

    // Cap: metallic vertical gradient, rounded, white indicator line.
    const float cy = r.bottom - r.Height() * frac;
    BRect cap(cx - Themed(12.0f), cy - Themed(6.0f),
              cx + Themed(12.0f), cy + Themed(6.0f));
    BGradientLinear grad(BPoint(cap.left, cap.top), BPoint(cap.left, cap.bottom));
    grad.AddColor(Rgb(96, 96, 106), 0);
    grad.AddColor(Rgb(62, 62, 70), 128);
    grad.AddColor(Rgb(44, 44, 52), 255);
    v->FillRoundRect(cap, Themed(2.0f), Themed(2.0f), grad);
    v->SetHighColor(ColKnobOutline());
    v->StrokeRoundRect(cap, Themed(2.0f), Themed(2.0f));
    v->SetHighColor(Rgb(232, 232, 238));
    v->StrokeLine(BPoint(cap.left + Themed(3.0f), cy),
                  BPoint(cap.right - Themed(3.0f), cy));
}

// The one meter bar (M3.3): a recessed well, a fill mapped through the dB
// scale of dsp/MeterMath.h and coloured in three bands by the theme's ONE
// threshold set (MeterColor: green to kMeterWarn, yellow to kMeterClip, red
// above) -- so every meter in the app reads the same level the same way. The
// old green->yellow->red gradient had its own stops at 0.6/0.8 of the height,
// which is exactly the second threshold set M1.2 removed.
//
// `level` is a linear amplitude (may exceed 1.0: the red band is the headroom
// above 0 dBFS). `rms` / `holdFrac` are optional overlays: a thin line at the
// RMS level and the peak-hold tick; pass < 0 for none. `horizontal` draws the
// bar growing to the right instead of upwards.
inline void DrawMeterBar(BView* v, BRect r, float level, bool horizontal,
                         float rms = -1.0f, float holdFrac = -1.0f) {
    v->SetHighColor(ColLcd());   // recessed well
    v->FillRect(r);

    const float frac = LinToFrac(level);
    const float warnFrac = LinToFrac(kMeterWarn);
    const float clipFrac = LinToFrac(kMeterClip);
    // Band boundaries as meter fractions, in the theme's colours.
    struct Band { float to; rgb_color col; };
    const Band bands[3] = {
        { warnFrac, MeterColor(0.0f) },
        { clipFrac, MeterColor(kMeterWarn) },
        { 1.0f,     MeterColor(kMeterClip) },
    };
    float from = 0.0f;
    for (const Band& b : bands) {
        const float to = frac < b.to ? frac : b.to;
        if (to > from) {
            BRect fill = r;
            if (horizontal) { fill.left  = r.left  + r.Width()  * from;
                              fill.right = r.left  + r.Width()  * to; }
            else            { fill.bottom = r.bottom - r.Height() * from;
                              fill.top    = r.bottom - r.Height() * to; }
            v->SetHighColor(b.col);
            v->FillRect(fill);
        }
        if (frac <= b.to) break;
        from = b.to;
    }

    if (rms >= 0.0f) {   // RMS line: a dark rule inside the fill, the way a
        const float f = LinToFrac(rms);   // peak/RMS bar reads at any level
        if (f > 0.0f) {
            v->SetHighColor(ColBackground());
            if (horizontal) {
                const float x = r.left + r.Width() * f;
                v->StrokeLine(BPoint(x, r.top + 1), BPoint(x, r.bottom - 1));
            } else {
                const float y = r.bottom - r.Height() * f;
                v->StrokeLine(BPoint(r.left + 1, y), BPoint(r.right - 1, y));
            }
        }
    }
    if (holdFrac > 0.0f) {   // peak-hold tick: white and thicker, so it reads
        const float f = holdFrac > 1.0f ? 1.0f : holdFrac;   // on every band
        v->SetHighColor(ColText());
        v->SetPenSize(2.0f);
        if (horizontal) {
            const float x = r.left + r.Width() * f;
            v->StrokeLine(BPoint(x, r.top + 1), BPoint(x, r.bottom - 1));
        } else {
            const float y = r.bottom - r.Height() * f;
            v->StrokeLine(BPoint(r.left + 1, y), BPoint(r.right - 1, y));
        }
        v->SetPenSize(1.0f);
    }

    v->SetHighColor(ColGrid());
    v->StrokeRect(r);
}

// The clip lamp: a small square that lights in the clip colour while the meter
// is latched, with a dim outline otherwise (so the user can see where to click
// to reset it).
inline void DrawClipLamp(BView* v, BRect r, bool clipped) {
    v->SetHighColor(clipped ? MeterColor(kMeterClip) : ColBtnOff());
    v->FillRect(r);
    v->SetHighColor(ColBtnBorder());
    v->StrokeRect(r);
}

// Linear gain <-> fader fraction. Unity (1.0) sits at kUnityFrac so there's
// headroom above; kMaxGainW is the top of the fader travel.
constexpr float kMaxGainW  = 1.5f;    // fader top = +3.5 dB
constexpr float kUnityFrac = 1.0f / kMaxGainW;   // where 1.0 gain sits

inline float GainToFrac(float gain) {
    float f = gain / kMaxGainW;
    if (!(f > 0)) return 0;   // NaN / negative -> 0
    return f > 1 ? 1 : f;
}
inline float FracToGain(float frac) {
    if (!(frac > 0)) frac = 0;   // NaN / negative -> 0
    if (frac > 1) frac = 1;
    return frac * kMaxGainW;
}
inline float GainToDb(float gain) {
    return gain > 1e-4f ? 20.0f * std::log10(gain) : -80.0f;
}

} // namespace daw
