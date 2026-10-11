#include "MarkersWindow.h"

#include "UiMetrics.h"          // Themed + the shared messages
#include "RenameWindow.h"       // the rename prompt (posts kMsgRenameMarker)
#include "widgets/DawButton.h"  // the kit (M1.3)

#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <SpaceLayoutItem.h>
#include <String.h>

#include <algorithm>

namespace daw {

// --- the list ---------------------------------------------------------------

static constexpr float kRowH     = 22.0f;   // design px
static constexpr float kHeaderH  = 16.0f;

MarkersView::MarkersView(BRect frame, std::vector<MarkerInfo> markers,
                         Frame playhead, BMessenger apply)
    : BView(frame, "markers", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
      fMarkers(std::move(markers)), fPlayhead(playhead), fApply(apply) {
    ApplyTheme();
}

void MarkersView::ApplyTheme() { SetViewColor(ColHeader()); }

void MarkersView::SetMarkers(std::vector<MarkerInfo> markers, Frame playhead) {
    fMarkers = std::move(markers);
    fPlayhead = playhead;
    // The selection is a ROW index, so a list that shrank under it must clamp
    // (a stale index would delete a marker nobody pointed at).
    if (fSelected >= (int)fMarkers.size()) fSelected = (int)fMarkers.size() - 1;
    const int visible = std::max(1, (int)((Bounds().Height()
                                           - Themed(kHeaderH) - Themed(4.0f))
                                          / Themed(kRowH)));
    if (fScroll > (int)fMarkers.size() - visible)
        fScroll = std::max(0, (int)fMarkers.size() - visible);
    Invalidate();
}

void MarkersView::Post(uint32 what) {
    BMessage m(what);
    fApply.SendMessage(&m);
}

void MarkersView::AddAtPlayhead() { Post(kMsgMarkerAdd); }

void MarkersView::DeleteSelected() {
    if (fSelected < 0 || fSelected >= (int)fMarkers.size()) return;
    BMessage m(kMsgMarkerDelete);
    m.AddInt64("frame", (int64)fMarkers[(size_t)fSelected].frame);
    m.AddString("name", fMarkers[(size_t)fSelected].name.c_str());
    fApply.SendMessage(&m);
}

void MarkersView::RenameSelected() {
    if (fSelected < 0 || fSelected >= (int)fMarkers.size()) return;
    const MarkerInfo& mk = fMarkers[(size_t)fSelected];
    // The prompt and its answer go to the MAIN window (it owns the model):
    // same message the ruler's menu uses, so a rename means one thing
    // everywhere.
    const BPoint at = ConvertToScreen(BPoint(Themed(10.0f),
                                             Themed(kHeaderH) + Themed(4.0f)));
    (new RenameWindow(BRect(at.x, at.y, at.x + 260, at.y + 74),
                      (TrackId)mk.frame, mk.name.c_str(), fApply,
                      kMsgRenameMarker, 0, "Rename Marker"))->Show();
}

float MarkersView::RowRectTop(int i) const {
    return Themed(kHeaderH) + Themed(2.0f)
         + (float)(i - fScroll) * Themed(kRowH);
}

int MarkersView::RowAt(BPoint where) const {
    if (where.y < Themed(kHeaderH)) return -1;
    const int i = fScroll
                + (int)((where.y - Themed(kHeaderH) - Themed(2.0f))
                        / Themed(kRowH));
    if (i < 0 || i >= (int)fMarkers.size()) return -1;
    // ...and the point has to be inside the row, not in the gap below the last.
    const float top = RowRectTop(i);
    return (where.y >= top && where.y <= top + Themed(kRowH)) ? i : -1;
}

void MarkersView::MouseDown(BPoint where) {
    int32 clicks = 1;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("clicks", &clicks);
    const int row = RowAt(where);
    if (row < 0) return;
    fSelected = row;
    Invalidate();
    // Click-to-locate: the row IS the seek, so one click both selects and
    // jumps (a list that only jumped on a double-click would make "click a
    // marker to go there" a two-step).
    BMessage jump(kMsgMarkerJump);
    jump.AddInt64("frame", (int64)fMarkers[(size_t)row].frame);
    fApply.SendMessage(&jump);
    if (clicks >= 2) RenameSelected();   // double-click: rename it
}

void MarkersView::MessageReceived(BMessage* msg) {
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        msg->FindFloat("be:wheel_delta_y", &dy);
        const int visible = std::max(1, (int)((Bounds().Height()
                                               - Themed(kHeaderH)
                                               - Themed(4.0f))
                                              / Themed(kRowH)));
        const int maxScroll = std::max(0, (int)fMarkers.size() - visible);
        const int step = dy > 0.0f ? 1 : (dy < 0.0f ? -1 : 0);
        const int next = std::min(maxScroll, std::max(0, fScroll + step));
        if (next != fScroll) {
            fScroll = next;
            Invalidate();
        }
        return;
    }
    BView::MessageReceived(msg);
}

void MarkersView::Draw(BRect update) {
    BRect b = Bounds();
    SetHighColor(ColHeader());
    FillRect(b);

    // Column captions: every pane names what it shows.
    SetHighColor(ColTextDim());
    DrawString("Marker", BPoint(Themed(10.0f), Themed(12.0f)));
    {
        const char* pos = "Position";
        const float w = StringWidth(pos);
        DrawString(pos, BPoint(b.right - Themed(8.0f) - w, Themed(12.0f)));
    }
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, Themed(kHeaderH)), BPoint(b.right, Themed(kHeaderH)));

    if (fMarkers.empty()) {
        // An empty pane says how to fill it.
        const char* hint = "No markers yet";
        const char* hint2 = "Press M in the arranger, or Add at Playhead";
        SetHighColor(ColText());
        DrawString(hint, BPoint((b.Width() - StringWidth(hint)) * 0.5f,
                                b.Height() * 0.5f - Themed(8.0f)));
        BFont small(be_plain_font);
        small.SetSize(Themed(10.5f));
        SetFont(&small);
        SetHighColor(ColTextDim());
        DrawString(hint2, BPoint((b.Width() - StringWidth(hint2)) * 0.5f,
                                 b.Height() * 0.5f + Themed(8.0f)));
        return;
    }

    for (int i = 0; i < (int)fMarkers.size(); i++) {
        const float top = RowRectTop(i);
        if (top > b.bottom || top + Themed(kRowH) < Themed(kHeaderH)) continue;
        const MarkerInfo& mk = fMarkers[(size_t)i];
        const BRect row(0, top, b.right, top + Themed(kRowH));
        const bool sel = (i == fSelected);
        SetHighColor(sel ? ColHeaderHi() : ((i & 1) ? ColLaneAlt() : ColLane()));
        FillRect(row);

        // The flag, the name, and the position column.
        SetHighColor(ColMidiAccent());
        BPoint flag[3] = { BPoint(Themed(6.0f), top + Themed(5.0f)),
                           BPoint(Themed(14.0f), top + Themed(9.0f)),
                           BPoint(Themed(6.0f), top + Themed(13.0f)) };
        FillPolygon(flag, 3);

        BString name(mk.name.c_str());
        const float posW = StringWidth(mk.time.c_str());
        const float nameRoom = b.Width() - Themed(20.0f) - posW - Themed(18.0f);
        if (nameRoom > Themed(16.0f))
            TruncateString(&name, B_TRUNCATE_END, nameRoom);
        SetHighColor(ColText());
        DrawString(name.String(), BPoint(Themed(20.0f), top + Themed(15.0f)));
        DrawString(mk.time.c_str(), BPoint(b.right - Themed(8.0f) - posW,
                                           top + Themed(15.0f)));

        if (sel) {
            SetHighColor(ColAccent());
            StrokeRect(row);
        }
        SetHighColor(ColGrid());
        StrokeLine(BPoint(Themed(2.0f), row.bottom), BPoint(b.right - Themed(2.0f),
                                                            row.bottom));
    }
}

// --- the window -------------------------------------------------------------

MarkersWindow::MarkersWindow(BRect frame, std::vector<MarkerInfo> markers,
                             Frame playhead, BMessenger apply)
    : BWindow(frame, "Markers", B_TITLED_WINDOW, B_FLOATING_APP_WINDOW_FEEL,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fApply(apply) {
    fView = new MarkersView(BRect(0, 0, 300, 220), std::move(markers), playhead,
                            fApply);
    fView->SetExplicitMinSize(BSize(Themed(240.0f), Themed(120.0f)));

    DawButton* add = new DawButton("mk-add", "Add at Playhead",
                                   new BMessage(kMsgMarkerAddBtn));
    DawButton* rename = new DawButton("mk-rename", "Rename" B_UTF8_ELLIPSIS,
                                      new BMessage(kMsgMarkerRenameBtn));
    DawButton* del = new DawButton("mk-delete", "Delete",
                                   new BMessage(kMsgMarkerDeleteBtn));

    ThemedGroupView* root = new ThemedGroupView("markers-root", B_VERTICAL,
                                                Themed(0.0f));
    BLayoutBuilder::Group<>(root)
        .SetInsets(Themed(6.0f), Themed(6.0f), Themed(6.0f), Themed(6.0f))
        .Add(fView, 1.0f)
        .AddGroup(B_HORIZONTAL, Themed(6.0f))
            .Add(add)
            .Add(rename)
            .Add(del)
            .AddGlue()
        .End();

    BLayoutBuilder::Group<>(this, B_VERTICAL).Add(root);
    SetSizeLimits(Themed(260.0f), 32767.0f, Themed(180.0f), 32767.0f);
    ThemeStockTopView(this);
}

void MarkersWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case kMsgMarkerAddBtn:
            if (fView) fView->AddAtPlayhead();
            break;
        case kMsgMarkerDeleteBtn:
            if (fView) fView->DeleteSelected();
            break;
        case kMsgMarkerRenameBtn:
            if (fView) fView->RenameSelected();
            break;
        case kMsgMarkerActivate:
            Activate();
            break;
        default:
            BWindow::MessageReceived(msg);
    }
}

void MarkersWindow::DispatchMessage(BMessage* msg, BHandler* handler) {
    // Space and Cmd-Z mean the same here as in every child window.
    if (ForwardSpaceToTransport(msg, fApply)) return;
    if (ForwardUndoRedo(msg, fApply)) return;
    BWindow::DispatchMessage(msg, handler);
}

} // namespace daw
