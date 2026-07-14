#include "TransportBar.h"

#include "UiMetrics.h"

namespace daw {

// Button rects (match the old layout so the other widgets keep their x).
static BRect PlayRect() { return BRect(6, 5, 70, 27); }
static BRect StopRect() { return BRect(74, 5, 138, 27); }
static BRect RecRect()  { return BRect(142, 5, 206, 27); }

TransportBar::TransportBar(BRect frame, BMessenger target,
                           uint32 playWhat, uint32 stopWhat, uint32 recWhat)
    : BView(frame, "transport", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW),
      fTarget(target), fPlay(playWhat), fStop(stopWhat), fRec(recWhat) {
    SetViewColor(ColHeader());
}

static void DrawButton(BView* v, BRect r, rgb_color fill, bool lit) {
    v->SetHighColor(lit ? fill : ColHeaderHi());
    v->FillRect(r);
    v->SetHighColor(lit ? fill : ColGrid());
    v->StrokeRect(r);
}

void TransportBar::Draw(BRect) {
    const rgb_color icon = ColText();

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
    DrawButton(this, rc, Rgb(180, 60, 60), fRecording);
    SetHighColor(fRecording ? Rgb(255, 210, 210) : Rgb(210, 80, 80));
    const float rcy = (rc.top + rc.bottom) / 2;
    FillEllipse(BPoint(rc.left + 32, rcy), 7, 7);

    // Divider before the readouts.
    SetHighColor(ColGrid());
    StrokeLine(BPoint(208, 3), BPoint(208, Bounds().Height() - 3));
}

void TransportBar::MouseDown(BPoint where) {
    if (PlayRect().Contains(where))      fTarget.SendMessage(fPlay);
    else if (StopRect().Contains(where)) fTarget.SendMessage(fStop);
    else if (RecRect().Contains(where))  fTarget.SendMessage(fRec);
}

} // namespace daw
