#include "MeterView.h"

#include "UiMetrics.h"
#include "Widgets.h"

#include <Messenger.h>
#include <OS.h>
#include <Window.h>

#include <cmath>

namespace daw {

MeterView::MeterView(BRect frame)
    : BView(frame, "meter", B_FOLLOW_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW) {
    SetViewColor(ColHeader());
}

void MeterView::SetReadings(const MeterReadings& r) {
    // Ignore sub-pixel changes so a quiet signal doesn't repaint every tick —
    // but keep repainting while a peak hold is still falling (it is drawn
    // between the bar and its tick, and it moves every tick).
    const bool moved = std::fabs(r.peakL - fIn.peakL) >= 0.01f
                    || std::fabs(r.peakR - fIn.peakR) >= 0.01f
                    || std::fabs(r.rmsL  - fIn.rmsL)  >= 0.01f
                    || std::fabs(r.rmsR  - fIn.rmsR)  >= 0.01f
                    || std::fabs(r.correlation - fIn.correlation) >= 0.01f
                    || r.clipL != fIn.clipL || r.clipR != fIn.clipR;

    fIn = r;
    fClipL.Latch(r.clipL);
    fClipR.Latch(r.clipR);

    const int64_t now = system_time();
    const float holdL0 = fHoldL.Frac(), holdR0 = fHoldR.Frac();
    fHoldL.Update(LinToFrac(r.peakL), now);
    fHoldR.Update(LinToFrac(r.peakR), now);
    fLastUpdateUs = now;

    const bool holding = std::fabs(holdL0 - fHoldL.Frac()) > 0.001f
                      || std::fabs(holdR0 - fHoldR.Frac()) > 0.001f;
    if (moved || holding) Invalidate();
}

void MeterView::ResetClip() {
    // The latch is ours; the flag bits are the engine's (and the recorder's).
    // The window clears those — a clip that happens between its read and its
    // clear is kept by the fetch_and there.
    fClipL.Reset();
    fClipR.Reset();
    if (BWindow* w = Window())
        BMessenger(w).SendMessage(kMsgMeterClipReset);
    Invalidate();
}

void MeterView::MouseDown(BPoint) {
    ResetClip();
}

void MeterView::Draw(BRect) {
    BRect b = Bounds();
    const float corrW = Themed(13.0f);
    const float lampW = Themed(9.0f);
    const float gap   = Themed(3.0f);
    const float barR  = b.right - corrW - gap - lampW - gap;
    const float mid   = (b.top + b.bottom) * 0.5f;

    const BRect barL(b.left + 1, b.top + 1, barR, mid - 1.0f);
    const BRect barRr(b.left + 1, mid + 1, barR, b.bottom - 1);

    DrawMeterBar(this, barL, fIn.peakL, true, fIn.rmsL, fHoldL.Frac());
    DrawMeterBar(this, barRr, fIn.peakR, true, fIn.rmsR, fHoldR.Frac());
    DrawScaleTicks(barL);   // dB scale marks, the same list on both bars
    DrawScaleTicks(barRr);

    // Clip lamps: one per channel, beside its bar.
    const float lampH = Themed(6.0f);
    BRect lampL(barR + gap, (barL.top + barL.bottom) * 0.5f - lampH * 0.5f,
                barR + gap + lampW, (barL.top + barL.bottom) * 0.5f + lampH * 0.5f);
    BRect lampR2(barR + gap, (barRr.top + barRr.bottom) * 0.5f - lampH * 0.5f,
                 barR + gap + lampW, (barRr.top + barRr.bottom) * 0.5f + lampH * 0.5f);
    DrawClipLamp(this, lampL, fClipL.Clipped());
    DrawClipLamp(this, lampR2, fClipR.Clipped());

    DrawCorrelation(BRect(b.right - corrW - 1, b.top + 1, b.right - 1,
                          b.bottom - 1));
}

// The dB marks, drawn inside a bar: hairlines at the metre's own scale points
// (MeterMath's kMeterTicksDb), so the bar reads as a dB meter rather than as a
// filled rectangle.
void MeterView::DrawScaleTicks(BRect bar) {
    SetHighColor(ColGrid());
    for (int i = 0; i < kMeterTickCount; i++) {
        const float x = bar.left + bar.Width() * DbToFrac(kMeterTicksDb[i]);
        StrokeLine(BPoint(x, bar.top), BPoint(x, bar.bottom));
    }
}

// The correlation strip: a well with the zero line, and an indicator growing
// from the centre towards + (in phase) or - (out of phase).
void MeterView::DrawCorrelation(BRect r) {
    SetHighColor(ColLcd());
    FillRect(r);
    SetHighColor(ColGrid());
    StrokeRect(r);

    const float mid = (r.top + r.bottom) * 0.5f;
    float c = fIn.correlation;
    if (!(c >= -1.0f)) c = 0.0f;      // NaN
    if (c >  1.0f) c =  1.0f;
    if (c < -1.0f) c = -1.0f;
    const float h = (r.Height() * 0.5f - Themed(1.0f)) * std::fabs(c);

    if (h > 0.5f) {
        SetHighColor(c >= 0.0f ? ColPlay() : ColRec());
        if (c >= 0.0f) FillRect(BRect(r.left + 1, mid - h, r.right - 1, mid));
        else           FillRect(BRect(r.left + 1, mid, r.right - 1, mid + h));
    }
    // The zero line sits over the fill, so a small value still reads.
    SetHighColor(ColTextDim());
    StrokeLine(BPoint(r.left + 1, mid), BPoint(r.right - 1, mid));
}

} // namespace daw
