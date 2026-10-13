#include "TransportBar.h"

#include "UiMetrics.h"
#include "Widgets.h"   // DrawButton (the shared drawn-button look)

#include <String.h>

#include <cstdio>
#include <cstring>

namespace daw {

// --- geometry (design pixels at kDesignFontSize; everything goes through
// Themed(), so the user's font size scales the whole strip) -------------------
//
// Two rows: the transport buttons and the LCD on top (y 5..31), the status
// line under them (y 33..51). The zoom pair sits at the right end of the
// button block; the LCD well takes the space after it. The window pins its own
// children (loudness, tempo, master volume, meter) to the same row grid — see
// MainWindow::LayoutTransportBar, which owns every offset that is NOT drawn
// here.
static constexpr float kBtnW    = 28.0f;
static constexpr float kBtnH    = 22.0f;
static constexpr float kBtnTop  = 6.0f;
// Left x of each button, in groups: locate | transport | modes. The 6 px gaps
// between groups are the only thing separating them, as on a console.
static constexpr float kBtnX[9] = { 6, 36, 66, 102, 132, 162, 198, 228, 258 };
static constexpr float kZoomW   = 26.0f;
static constexpr float kZoomH   = 20.0f;
static constexpr float kZoomTop = 7.0f;
static constexpr float kZoomX[2] = { 294.0f, 322.0f };

static constexpr float kLcdL = 356.0f;
static constexpr float kLcdR = 590.0f;
static constexpr float kLcdT = 4.0f;    // the well's top inset
static constexpr float kLcdB = 4.0f;    // ... and bottom

// Row B's baseline and the left inset the status text starts at.
static constexpr float kStatusX       = 6.0f;
static constexpr float kStatusBaseline = 46.0f;

static const TransportButton kButtonOrder[11] = {
    TransportButton::Rtz,    TransportButton::Rewind, TransportButton::Forward,
    TransportButton::Play,   TransportButton::Stop,   TransportButton::Rec,
    TransportButton::Loop,   TransportButton::Click,  TransportButton::Punch,
    TransportButton::ZoomOut, TransportButton::ZoomIn,
};

BRect ButtonRect(TransportButton which) {
    const int i = (int)which;
    if (i < 0 || i >= (int)TransportButton::Count) return BRect();
    if (which == TransportButton::ZoomOut || which == TransportButton::ZoomIn) {
        const int k = (which == TransportButton::ZoomOut) ? 0 : 1;
        return BRect(Themed(kZoomX[k]), Themed(kZoomTop),
                     Themed(kZoomX[k] + kZoomW), Themed(kZoomTop + kZoomH));
    }
    return BRect(Themed(kBtnX[i]), Themed(kBtnTop),
                 Themed(kBtnX[i] + kBtnW), Themed(kBtnTop + kBtnH));
}

BRect LcdWellRect(float barHeight) {
    return BRect(Themed(kLcdL), Themed(kLcdT), Themed(kLcdR),
                 barHeight - Themed(kLcdB));
}

// --- the big position display ----------------------------------------------

TransportTimeView::TransportTimeView(BRect frame, BMessenger target)
    : BView(frame, "time", B_FOLLOW_LEFT | B_FOLLOW_TOP,
            B_WILL_DRAW | B_NAVIGABLE),
      fTarget(target) {
    ApplyTheme();
}

// The well is the theme's LCD token (T1): the walker reaches this child of the
// bar, so a mode switch re-colours the display with the rest of the strip.
void TransportTimeView::ApplyTheme() {
    SetViewColor(ColLcd());
}

void TransportTimeView::SetPosition(const char* text, const char* tag) {
    const char* t = text ? text : "";
    const char* g = tag ? tag : "";
    if (fText == t && fTag == g) return;
    fText = t;
    fTag  = g;
    Invalidate();
}

void TransportTimeView::SetInfoLine(const char* text) {
    const char* t = text ? text : "";
    if (fInfo == t) return;
    fInfo = t;
    Invalidate();
}

void TransportTimeView::MakeFocus(bool focused) {
    if (!focused && fEditing) {
        // Focus left the view (a click somewhere else): the entry is abandoned,
        // never applied half-typed. The clear has to reach the screen: without
        // the invalidate the abandoned digits and the caret stayed drawn until
        // something else happened to repaint the bar.
        fEditing = false;
        fTyped.clear();
        Invalidate();
    }
    BView::MakeFocus(focused);
}

void TransportTimeView::MouseDown(BPoint where) {
    BMessage* msg = Window() ? Window()->CurrentMessage() : nullptr;
    int32 clicks = 1;
    if (msg) msg->FindInt32("clicks", &clicks);
    if (clicks >= 2 && !fEditing) {
        // Start a typed entry: what the keyboard types is shown here, and Enter
        // hands it to the window, which owns the mode and the tempo map.
        fEditing = true;
        fTyped.clear();
        MakeFocus(true);
        Invalidate();
        return;
    }
    if (fEditing) {   // a single click while editing ends the gesture
        fEditing = false;
        fTyped.clear();
        MakeFocus(false);   // ... and gives the keyboard back (see KeyDown)
        Invalidate();
        return;
    }
    // Cycle the format. The WINDOW owns the mode (it formats the text), so the
    // click asks it to advance rather than switching a second copy here.
    if (fTarget.IsValid()) fTarget.SendMessage(kMsgTimeCycle);
}

void TransportTimeView::KeyDown(const char* bytes, int32 numBytes) {
    if (!fEditing) {
        // Not editing: the transport keys belong to the window (the timeline
        // resolves them through the one key map, M3.2).
        BView::KeyDown(bytes, numBytes);
        return;
    }
    for (int i = 0; i < numBytes; i++) {
        const char c = bytes[i];
        if (c == B_RETURN) {
            BMessage m(kMsgTimeEntry);
            m.AddString("text", fTyped.c_str());
            if (Window()) Window()->PostMessage(&m);
            fEditing = false;
            fTyped.clear();
            // Hand the keyboard back: the window routes a key to the timeline
            // unless something else holds the focus, and a view that keeps it
            // after the entry is over swallows R, L, C, Home and the arrows
            // until the user clicks the timeline again.
            MakeFocus(false);
            Invalidate();
            return;
        }
        if (c == B_ESCAPE) {
            fEditing = false;
            fTyped.clear();
            MakeFocus(false);
            Invalidate();
            return;
        }
        if (c == B_BACKSPACE) {
            if (!fTyped.empty()) fTyped.pop_back();
            Invalidate();
            continue;
        }
        // Digits and the separators a position is written with; anything else
        // is not part of a time and is dropped rather than rejected (the
        // window's parse is the one that decides).
        if ((c >= '0' && c <= '9') || c == ':' || c == '.' || c == ' ') {
            if (fTyped.size() < 24) fTyped.push_back(c);
            Invalidate();
        }
    }
}

void TransportTimeView::Draw(BRect) {
    const BRect b = Bounds();

    // The well's recessed background and edge.
    SetHighColor(ColLcd());
    FillRect(b);
    SetHighColor(ColGrid());
    StrokeRect(b);

    BFont small(be_plain_font);
    small.SetSize(Themed(10.5f));
    BFont big(be_bold_font);
    big.SetSize(Themed(17.0f));

    // The mode tag, at the well's left edge, on the position's own line.
    SetFont(&small);
    SetHighColor(ColTextDim());
    const char* tag = fTag.empty() ? "" : fTag.c_str();
    const float tagW = StringWidth(tag);
    DrawString(tag, BPoint(b.left + Themed(6.0f), b.top + Themed(21.0f)));

    // The position itself: centred in what the tag leaves, or the typed entry
    // while one is in progress (with a caret, so it is obvious that typing
    // goes here).
    SetFont(&big);
    SetHighColor(ColLcdText());
    std::string shown = fEditing ? fTyped : fText;
    if (fEditing) shown += "_";
    if (shown.empty()) shown = "--";
    const float textL = b.left + Themed(6.0f) + tagW + Themed(8.0f);
    const float textR = b.right - Themed(6.0f);
    float tw = StringWidth(shown.c_str());
    float x = textL + (textR - textL - tw) * 0.5f;
    if (x < textL) x = textL;
    DrawString(shown.c_str(), BPoint(x, b.top + Themed(26.0f)));

    // The readout line (tempo/meter/DSP/xrun), set by the window.
    if (!fInfo.empty()) {
        SetFont(&small);
        SetHighColor(ColTextDim());
        BString s(fInfo.c_str());
        TruncateString(&s, B_TRUNCATE_END, b.Width() - Themed(12.0f));
        DrawString(s.String(), BPoint(b.left + Themed(6.0f),
                                     b.bottom - Themed(4.0f)));
    }
}

// --- the bar ---------------------------------------------------------------

TransportBar::TransportBar(BRect frame, BMessenger target,
                           const TransportActions& actions)
    : BView(frame, "transport", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP,
            B_WILL_DRAW | B_SUPPORTS_LAYOUT),
      fTarget(target), fAct(actions) {
    ApplyTheme();
    fTime = new TransportTimeView(LcdWellRect(frame.Height()), target);
    AddChild(fTime);
}

void TransportBar::ApplyTheme() {
    SetViewColor(ColChrome());
}

void TransportBar::GetPreferredSize(float* width, float* height) {
    // Its height is a fixed strip (whose CONTENTS are pinned offsets, see
    // MainWindow::LayoutTransportBar); the width is whatever the window gives.
    if (width)  *width  = 0.0f;
    if (height) *height = Themed(kDesignTransportH);
}

// The preferred height alone is only a hint: a plain BView's maximum is
// unlimited, so the window's group layout handed the bar a share of every
// spare pixel and the strip grew a dead band under the buttons. Min and max
// pin it to the strip's height; the width still follows the window.
BSize TransportBar::MinSize() {
    return BSize(0.0f, Themed(kDesignTransportH));
}

BSize TransportBar::MaxSize() {
    return BSize(B_SIZE_UNLIMITED, Themed(kDesignTransportH));
}

void TransportBar::SetStatus(const char* text) {
    const char* t = text ? text : "";
    if (fStatus == t) return;
    fStatus = t;
    Invalidate();
}

void TransportBar::SetInfo(const char* text) {
    const char* t = text ? text : "";
    if (fTime) fTime->SetInfoLine(t);
}

void TransportBar::SetLcdVisible(bool on) {
    if (!fTime || fLcdOn == on) return;
    fLcdOn = on;
    if (on) fTime->Show(); else fTime->Hide();
    Invalidate();
}

// The glyphs. Every button keeps the drawn-button look (Widgets.h) so the bar
// reads like the rest of the app; the glyph is what differs, and a lit button
// draws it in the fill's own contrast colour.
static void DrawGlyph(BView* v, TransportButton which, BRect r, bool lit,
                      rgb_color fill) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = (r.top + r.bottom) * 0.5f;
    v->SetHighColor(lit ? LabelOn(fill) : ColText());
    auto tri = [&](float x, float dir, float h, float w) {
        BPoint p[3] = { BPoint(x, cy - h), BPoint(x, cy + h),
                        BPoint(x + dir * w, cy) };
        v->FillPolygon(p, 3);
    };
    switch (which) {
        case TransportButton::Rtz:
            v->FillRect(BRect(r.left + 8, cy - 7, r.left + 10, cy + 7));
            tri(r.left + 19, -1, 6, 9);
            break;
        case TransportButton::Rewind:
            tri(r.left + 14, -1, 6, 8);
            tri(r.left + 23, -1, 6, 8);
            break;
        case TransportButton::Forward:
            tri(r.left + 14, 1, 6, 8);
            tri(r.left + 23, 1, 6, 8);
            break;
        case TransportButton::Play:
            tri(r.left + 10, 1, 8, 11);
            break;
        case TransportButton::Stop:
            v->FillRect(BRect(cx - 6, cy - 6, cx + 6, cy + 6));
            break;
        case TransportButton::Rec:
            v->FillEllipse(BPoint(cx, cy), 6, 6);
            break;
        case TransportButton::Loop: {
            // A circular arrow. A full ring with a solid head on it reads as
            // "loop" at 22 px; the first version was an open arc whose head was
            // lost in the button, so the whole glyph read as the letter C
            // (found by the screenshot pass).
            v->SetPenSize(2.0f);
            v->StrokeEllipse(BPoint(cx, cy), 6, 6);
            v->SetPenSize(1.0f);
            BPoint head[3] = { BPoint(cx + 8, cy - 7), BPoint(cx + 1, cy - 7),
                               BPoint(cx + 5, cy - 1) };
            v->FillPolygon(head, 3);
            break;
        }
        case TransportButton::Click: {
            // The metronome: the tapered body every metronome icon has, its
            // base, and the pendulum leaning to one side. (A bare triangle read
            // as nothing at all -- the screenshot pass.)
            BPoint body[4] = { BPoint(cx - 6, cy + 5), BPoint(cx - 3, cy - 6),
                               BPoint(cx + 3, cy - 6), BPoint(cx + 6, cy + 5) };
            v->StrokePolygon(body, 4);
            v->FillRect(BRect(cx - 8, cy + 6, cx + 8, cy + 8));
            v->StrokeLine(BPoint(cx + 2, cy - 6), BPoint(cx - 4, cy + 3));
            break;
        }
        case TransportButton::Punch:
            // Record between markers: | [ ] |.
            v->FillRect(BRect(r.left + 7, cy - 7, r.left + 9, cy + 7));
            v->FillRect(BRect(r.right - 9, cy - 7, r.right - 7, cy + 7));
            v->FillEllipse(BPoint(cx, cy), 5, 5);
            break;
        case TransportButton::ZoomOut:
        case TransportButton::ZoomIn:
            v->FillRect(BRect(cx - 5, cy - 1, cx + 5, cy + 1));
            if (which == TransportButton::ZoomIn)
                v->FillRect(BRect(cx - 1, cy - 5, cx + 1, cy + 5));
            break;
        default:
            break;
    }
}

void TransportBar::Draw(BRect) {
    // Row B first: the status text under the buttons. It is drawn even when
    // the buttons' own row is busy, so a message is never hidden by a repaint
    // of the transport alone. Truncated to the BUTTON BLOCK's width, not the
    // bar's: the LCD well starts at x 356 and a long message must stop there
    // rather than run under it (nothing overlaps — UI_GUIDELINES §3).
    if (!fStatus.empty()) {
        SetHighColor(ColTextDim());
        BString s(fStatus.c_str());
        TruncateString(&s, B_TRUNCATE_END, Themed(kLcdL) - Themed(kStatusX)
                                           - Themed(8.0f));
        DrawString(s.String(), BPoint(Themed(kStatusX), Themed(kStatusBaseline)));
    }

    for (TransportButton which : kButtonOrder) {
        const BRect r = ButtonRect(which);
        if (which == TransportButton::ZoomOut || which == TransportButton::ZoomIn) {
            DrawButton(this, r, "", false, ColChromeHi());
            SetHighColor(ColGrid());
            StrokeRoundRect(r, Themed(4.0f), Themed(4.0f));
            DrawGlyph(this, which, r, false, ColChromeHi());
            continue;
        }
        // Each button shows its own state (M3.1). The lit fill is the same
        // colour the app already uses for that state elsewhere.
        rgb_color fill = ColChromeHi();
        bool lit = false;
        switch (which) {
            case TransportButton::Play:  fill = ColPlay();  lit = fPlaying;   break;
            case TransportButton::Rec:   fill = ColRec();   lit = fRecording; break;
            case TransportButton::Loop:  fill = ColAccent();lit = fLoop;      break;
            case TransportButton::Click: fill = ColSolo();  lit = fClick;     break;
            case TransportButton::Punch: fill = ColRec();   lit = fPunch;     break;
            default: break;
        }
        DrawButton(this, r, "", lit, fill);
        DrawGlyph(this, which, r, lit, fill);
    }
}

void TransportBar::FrameResized(float width, float height) {
    BView::FrameResized(width, height);
    // The window re-places the pinned children (LayoutTransportBar). It is not
    // a formality: a pane re-layout can widen the bar without the window
    // resizing, and without this the master meter -- pinned to the bar's right
    // edge -- was left outside the window, invisible, until the next window
    // resize. Found by the M3.3 screenshot pass.
    if (fTime) {
        const BRect well = LcdWellRect(height);
        fTime->MoveTo(well.left, well.top);
        fTime->ResizeTo(well.Width(), well.Height());
    }
    fTarget.SendMessage(kMsgTransportLayout);
}

TransportButton TransportBar::ButtonAt(BPoint where) const {
    for (TransportButton which : kButtonOrder)
        if (ButtonRect(which).Contains(where)) return which;
    return TransportButton::Count;
}

uint32 TransportBar::MessageFor(TransportButton which) const {
    switch (which) {
        case TransportButton::Rtz:     return fAct.rtz;
        case TransportButton::Rewind:  return fAct.rewind;
        case TransportButton::Forward: return fAct.forward;
        case TransportButton::Play:    return fAct.play;
        case TransportButton::Stop:    return fAct.stop;
        case TransportButton::Rec:     return fAct.rec;
        case TransportButton::Loop:    return fAct.loop;
        case TransportButton::Click:   return fAct.click;
        case TransportButton::Punch:   return fAct.punch;
        case TransportButton::ZoomOut: return fAct.zoomOut;
        case TransportButton::ZoomIn:  return fAct.zoomIn;
        default: return 0;
    }
}

void TransportBar::MouseDown(BPoint where) {
    const TransportButton which = ButtonAt(where);
    if (which == TransportButton::Count) return;
    const uint32 what = MessageFor(which);
    if (what != 0) fTarget.SendMessage(what);
}

} // namespace daw
