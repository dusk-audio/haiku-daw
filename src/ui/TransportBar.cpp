#include "TransportBar.h"

#include "UiMetrics.h"

namespace daw {

// Button rects (match the layout so the other widgets keep their x).
static BRect PlayRect()    { return BRect(6, 5, 70, 27); }
static BRect StopRect()    { return BRect(74, 5, 138, 27); }
static BRect RecRect()     { return BRect(142, 5, 206, 27); }
static BRect ZoomOutRect() { return BRect(366, 6, 392, 27); }
static BRect ZoomInRect()  { return BRect(394, 6, 420, 27); }
// LCD display well around the bar/beat + time readout only.
static BRect LcdRect(float h) { return BRect(212, 4, 360, h - 4); }

TransportBar::TransportBar(BRect frame, BMessenger target,
                           uint32 playWhat, uint32 stopWhat, uint32 recWhat,
                           uint32 zoomOutWhat, uint32 zoomInWhat)
    : BView(frame, "transport", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW),
      fTarget(target), fPlay(playWhat), fStop(stopWhat), fRec(recWhat),
      fZoomOut(zoomOutWhat), fZoomIn(zoomInWhat) {
    SetViewColor(ColChrome());
}

static void DrawButton(BView* v, BRect r, rgb_color fill, bool lit) {
    v->SetHighColor(lit ? fill : ColChromeHi());
    v->FillRoundRect(r, 4, 4);
    v->SetHighColor(lit ? fill : ColGrid());
    v->StrokeRoundRect(r, 4, 4);
}

void TransportBar::Draw(BRect) {
    const rgb_color icon = ColText();
    const float H = Bounds().Height();

    // LCD-style display well behind the bar/beat + time readout (Logic look).
    SetHighColor(ColLcd());
    FillRoundRect(LcdRect(H), 5, 5);
    SetHighColor(ColGrid());
    StrokeRoundRect(LcdRect(H), 5, 5);

    // Play: triangle, lit green when playing.
    BRect p = PlayRect();
    DrawButton(this, p, Rgb(60, 140, 90), fPlaying);
    SetHighColor(fPlaying ? ColBackground() : Rgb(90, 200, 130));
    const float cy = (p.top + p.bottom) / 2;
    BPoint tri[3] = { BPoint(p.left + 24, cy - 7), BPoint(p.left + 24, cy + 7),
                      BPoint(p.left + 38, cy) };
    FillPolygon(tri, 3);

    // Stop: square.
    BRect s = StopRect();
    DrawButton(this, s, ColHeaderHi(), false);
    SetHighColor(icon);
    const float scy = (s.top + s.bottom) / 2;
    FillRect(BRect(s.left + 26, scy - 6, s.left + 38, scy + 6));

    // Rec: circle, lit red when recording.
    BRect rc = RecRect();
    DrawButton(this, rc, Rgb(200, 62, 62), fRecording);
    SetHighColor(fRecording ? Rgb(255, 210, 210) : Rgb(220, 84, 84));
    const float rcy = (rc.top + rc.bottom) / 2;
    FillEllipse(BPoint(rc.left + 32, rcy), 7, 7);

    // Zoom out / in: small dark buttons with legible glyphs.
    auto zoomBtn = [&](BRect r, char glyph) {
        SetHighColor(ColChromeHi());
        FillRoundRect(r, 4, 4);
        SetHighColor(ColGrid());
        StrokeRoundRect(r, 4, 4);
        SetHighColor(Rgb(226, 230, 236));
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        FillRect(BRect(cx - 5, cy - 1, cx + 5, cy + 1));       // minus bar
        if (glyph == '+')
            FillRect(BRect(cx - 1, cy - 5, cx + 1, cy + 5));   // plus stem
    };
    zoomBtn(ZoomOutRect(), '-');
    zoomBtn(ZoomInRect(),  '+');
}

void TransportBar::MouseDown(BPoint where) {
    if (PlayRect().Contains(where))         fTarget.SendMessage(fPlay);
    else if (StopRect().Contains(where))    fTarget.SendMessage(fStop);
    else if (RecRect().Contains(where))     fTarget.SendMessage(fRec);
    else if (ZoomOutRect().Contains(where)) fTarget.SendMessage(fZoomOut);
    else if (ZoomInRect().Contains(where))  fTarget.SendMessage(fZoomIn);
}

} // namespace daw
