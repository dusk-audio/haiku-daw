// DawControlLook — see the header. Every drawing here ignores the `base`
// colour the caller passes (the view's own colour, which may be the light
// system default) and paints the theme's instead: that is the whole point of
// installing a look, the app decides what a control looks like.
#include "DawControlLook.h"

#include "../Theme.h"

#include <Control.h>
#include <View.h>

#include <cmath>

namespace daw {

// --- helpers -------------------------------------------------------------

void DawControlLook::FillRounded(BView* view, const BRect& rect, float radius,
                                 const rgb_color& color) {
    if (!rect.IsValid()) return;
    view->SetHighColor(color);
    if (radius > 0.0f) view->FillRoundRect(rect, radius, radius);
    else               view->FillRect(rect);
}

void DawControlLook::StrokeRounded(BView* view, const BRect& rect, float radius,
                                   const rgb_color& color) {
    if (!rect.IsValid()) return;
    view->SetHighColor(color);
    const float pen = Themed(1.0f);
    view->SetPenSize(pen);
    BRect r = rect;
    r.InsetBy(pen * 0.5f, pen * 0.5f);
    if (radius > 0.0f) view->StrokeRoundRect(r, radius, radius);
    else               view->StrokeRect(r);
    view->SetPenSize(1.0f);
}

void DawControlLook::DrawFrame(BView* view, BRect& rect, float radius,
                               const rgb_color& background, uint32 flags,
                               const rgb_color& borderColor) {
    rgb_color fill = background;
    if ((flags & B_DISABLED) != 0)      fill = ColGrid();
    else if ((flags & B_CLICKED) != 0)  fill = ColHeaderHi();

    FillRounded(view, rect, radius, fill);

    rgb_color border = borderColor;
    if ((flags & B_DEFAULT_BUTTON) != 0)  border = ColAccent();
    if ((flags & B_FOCUSED) != 0)         border = ColAccent();
    if ((flags & B_DISABLED) != 0)        border = ColGrid();
    StrokeRounded(view, rect, radius, border);
    if ((flags & B_FOCUSED) != 0) {
        BRect inner = rect;
        inner.InsetBy(Themed(3.0f), Themed(3.0f));
        StrokeRounded(view, inner, Themed(2.0f), ColAccent());
    }
}

void DawControlLook::DrawArrow(BView* view, const BRect& rect, uint32 direction,
                               const rgb_color& color) {
    if (!rect.IsValid()) return;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float s = std::min(rect.Width(), rect.Height()) * 0.28f;
    BPoint p[3];
    switch (direction) {
        case B_LEFT_ARROW:
            p[0] = BPoint(cx + s * 0.6f, cy - s); p[1] = BPoint(cx + s * 0.6f, cy + s);
            p[2] = BPoint(cx - s * 0.6f, cy); break;
        case B_RIGHT_ARROW:
            p[0] = BPoint(cx - s * 0.6f, cy - s); p[1] = BPoint(cx - s * 0.6f, cy + s);
            p[2] = BPoint(cx + s * 0.6f, cy); break;
        case B_UP_ARROW:
            p[0] = BPoint(cx - s, cy + s * 0.6f); p[1] = BPoint(cx + s, cy + s * 0.6f);
            p[2] = BPoint(cx, cy - s * 0.6f); break;
        default:   // B_DOWN_ARROW and the corner arrows fall back to it
            p[0] = BPoint(cx - s, cy - s * 0.6f); p[1] = BPoint(cx + s, cy - s * 0.6f);
            p[2] = BPoint(cx, cy + s * 0.6f); break;
    }
    view->SetHighColor(color);
    view->FillPolygon(p, 3);
}

void DawControlLook::DrawPopupIndicator(BView* view, const BRect& rect, bool on) {
    if (!on) return;
    BRect box(rect.right - Themed(14.0f), rect.top,
              rect.right - Themed(2.0f), rect.bottom);
    DrawArrow(view, box, B_DOWN_ARROW, ColAccent());
}

// --- metrics -------------------------------------------------------------

DawControlLook::DawControlLook() {}
DawControlLook::~DawControlLook() {}

BAlignment DawControlLook::DefaultLabelAlignment() const {
    return BAlignment(B_ALIGN_LEFT, B_ALIGN_VERTICAL_CENTER);
}

float DawControlLook::DefaultLabelSpacing() const { return Themed(4.0f); }
float DawControlLook::DefaultItemSpacing() const  { return Themed(8.0f); }

uint32 DawControlLook::Flags(BControl* control) const {
    uint32 flags = 0;
    if (control != nullptr && !control->IsEnabled()) flags |= B_DISABLED;
    return flags;
}

void DawControlLook::GetFrameInsets(frame_type, uint32, float& left, float& top,
                                    float& right, float& bottom) {
    left = top = right = bottom = Themed(2.0f);
}

void DawControlLook::GetBackgroundInsets(background_type, uint32, float& left,
                                         float& top, float& right,
                                         float& bottom) {
    left = top = right = bottom = Themed(1.0f);
}

// --- buttons -------------------------------------------------------------

void DawControlLook::DrawButtonFrame(BView* view, BRect& rect,
        const BRect&, const rgb_color&, const rgb_color& background,
        uint32 flags, uint32) {
    DrawFrame(view, rect, Themed(3.0f), background, flags, ColBtnBorder());
}

void DawControlLook::DrawButtonFrame(BView* view, BRect& rect, const BRect&,
        float radius, const rgb_color&, const rgb_color& background,
        uint32 flags, uint32) {
    DrawFrame(view, rect, radius, background, flags, ColBtnBorder());
}

void DawControlLook::DrawButtonFrame(BView* view, BRect& rect, const BRect&,
        float, float, float, float, const rgb_color&,
        const rgb_color& background, uint32 flags, uint32) {
    // The corner-radius variants only matter for segmented bars, which the app
    // does not use: one radius for all four corners is the honest answer.
    DrawFrame(view, rect, Themed(3.0f), background, flags, ColBtnBorder());
}

void DawControlLook::DrawButtonBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, uint32,
        orientation) {
    DrawFrame(view, rect, Themed(3.0f), ColBtnOff(), flags, ColBtnBorder());
}

void DawControlLook::DrawButtonBackground(BView* view, BRect& rect,
        const BRect&, float radius, const rgb_color&, uint32 flags, uint32,
        orientation) {
    DrawFrame(view, rect, radius, ColBtnOff(), flags, ColBtnBorder());
}

void DawControlLook::DrawButtonBackground(BView* view, BRect& rect,
        const BRect&, float, float, float, float, const rgb_color&,
        uint32 flags, uint32, orientation) {
    DrawFrame(view, rect, Themed(3.0f), ColBtnOff(), flags, ColBtnBorder());
}

void DawControlLook::DrawButtonWithPopUpBackground(BView* view, BRect& rect,
        const BRect& updateRect, const rgb_color& base, uint32 flags,
        uint32 borders, orientation orientation) {
    DrawButtonBackground(view, rect, updateRect, base, flags, borders,
                         orientation);
}

void DawControlLook::DrawButtonWithPopUpBackground(BView* view, BRect& rect,
        const BRect& updateRect, float radius, const rgb_color& base,
        uint32 flags, uint32 borders, orientation orientation) {
    DrawButtonBackground(view, rect, updateRect, radius, base, flags, borders,
                         orientation);
}

void DawControlLook::DrawButtonWithPopUpBackground(BView* view, BRect& rect,
        const BRect& updateRect, float lt, float rt, float lb, float rb,
        const rgb_color& base, uint32 flags, uint32 borders,
        orientation orientation) {
    DrawButtonBackground(view, rect, updateRect, lt, rt, lb, rb, base, flags,
                         borders, orientation);
}

// --- menus ---------------------------------------------------------------

void DawControlLook::DrawMenuBarBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, uint32) {
    FillRounded(view, rect, 0.0f, (flags & B_DISABLED) ? ColGrid() : ColChrome());
}

void DawControlLook::DrawMenuBackground(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags, uint32) {
    FillRounded(view, rect, 0.0f,
                (flags & B_DISABLED) ? ColGrid() : ColHeader());
    StrokeRounded(view, rect, 0.0f, ColBtnBorder());
}

void DawControlLook::DrawMenuItemBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, uint32) {
    if ((flags & B_ACTIVATED) != 0) {
        FillRounded(view, rect, Themed(2.0f), ColAccent());
        return;
    }
    // Unselected items leave the menu's own background: painting each row
    // would band the panel.
    if ((flags & B_DISABLED) == 0) return;
    FillRounded(view, rect, Themed(2.0f), ColHeader());
}

void DawControlLook::DrawMenuFieldFrame(BView* view, BRect& rect, const BRect&,
        const rgb_color&, const rgb_color&, uint32 flags, uint32) {
    DrawFrame(view, rect, Themed(2.0f), ColLcd(), flags, ColBtnBorder());
}

void DawControlLook::DrawMenuFieldFrame(BView* view, BRect& rect, const BRect&,
        float radius, const rgb_color&, const rgb_color&, uint32 flags,
        uint32) {
    DrawFrame(view, rect, radius, ColLcd(), flags, ColBtnBorder());
}

void DawControlLook::DrawMenuFieldFrame(BView* view, BRect& rect, const BRect&,
        float, float, float, float, const rgb_color&, const rgb_color&,
        uint32 flags, uint32) {
    DrawFrame(view, rect, Themed(2.0f), ColLcd(), flags, ColBtnBorder());
}

void DawControlLook::DrawMenuFieldBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, bool popupIndicator, uint32 flags) {
    DrawFrame(view, rect, Themed(2.0f), ColLcd(), flags, ColBtnBorder());
    DrawPopupIndicator(view, rect, popupIndicator);
}

void DawControlLook::DrawMenuFieldBackground(BView* view, BRect& rect,
        const BRect&, float radius, const rgb_color&, bool popupIndicator,
        uint32 flags) {
    DrawFrame(view, rect, radius, ColLcd(), flags, ColBtnBorder());
    DrawPopupIndicator(view, rect, popupIndicator);
}

void DawControlLook::DrawMenuFieldBackground(BView* view, BRect& rect,
        const BRect&, float, float, float, float, const rgb_color&,
        bool popupIndicator, uint32 flags) {
    DrawFrame(view, rect, Themed(2.0f), ColLcd(), flags, ColBtnBorder());
    DrawPopupIndicator(view, rect, popupIndicator);
}

void DawControlLook::DrawMenuFieldBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, uint32) {
    FillRounded(view, rect, Themed(2.0f),
                (flags & B_DISABLED) ? ColGrid() : ColLcd());
}

// --- status bar, checks, radios -----------------------------------------

void DawControlLook::DrawStatusBar(BView* view, BRect& rect, const BRect&,
        const rgb_color&, const rgb_color&, float progressPosition) {
    FillRounded(view, rect, Themed(3.0f), ColLcd());
    const float w = rect.Width() * (progressPosition < 0 ? 0 :
                                    (progressPosition > 1 ? 1 : progressPosition));
    if (w > 0.0f) {
        BRect fill = rect;
        fill.right = fill.left + w;
        FillRounded(view, fill, Themed(3.0f), ColAccent());
    }
    StrokeRounded(view, rect, Themed(3.0f), ColBtnBorder());
}

void DawControlLook::DrawCheckBox(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags) {
    const bool on = (flags & B_ACTIVATED) != 0;
    FillRounded(view, rect, Themed(2.0f),
                on ? ColAccent() : ((flags & B_DISABLED) != 0 ? ColGrid()
                                                              : ColLcd()));
    StrokeRounded(view, rect, Themed(2.0f),
                  on ? ColAccent() : ColBtnBorder());
    if (on) {
        view->SetHighColor(Rgb(255, 255, 255));
        view->SetPenSize(Themed(1.5f));
        const float w = rect.Width(), h = rect.Height();
        view->StrokeLine(BPoint(rect.left + w * 0.24f, rect.top + h * 0.52f),
                         BPoint(rect.left + w * 0.44f, rect.top + h * 0.72f));
        view->StrokeLine(BPoint(rect.left + w * 0.44f, rect.top + h * 0.72f),
                         BPoint(rect.left + w * 0.78f, rect.top + h * 0.30f));
        view->SetPenSize(1.0f);
    }
}

void DawControlLook::DrawRadioButton(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags) {
    const bool on = (flags & B_ACTIVATED) != 0;
    const BPoint c((rect.left + rect.right) * 0.5f,
                   (rect.top + rect.bottom) * 0.5f);
    const float r = std::min(rect.Width(), rect.Height()) * 0.5f;
    view->SetHighColor(on ? ColAccent()
                          : ((flags & B_DISABLED) != 0 ? ColGrid() : ColLcd()));
    view->FillEllipse(c, r, r);
    view->SetHighColor(on ? ColAccent() : ColBtnBorder());
    view->StrokeEllipse(c, r, r);
    if (on) {
        view->SetHighColor(Rgb(255, 255, 255));
        view->FillEllipse(c, r * 0.42f, r * 0.42f);
    }
}

// --- scrollbars ----------------------------------------------------------

void DawControlLook::DrawScrollBarBackground(BView* view, BRect& rect1,
        BRect& rect2, const BRect&, const rgb_color&, uint32, orientation) {
    FillRounded(view, rect1, 0.0f, ColGrid());
    FillRounded(view, rect2, 0.0f, ColGrid());
}

void DawControlLook::DrawScrollBarBackground(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32, orientation) {
    FillRounded(view, rect, 0.0f, ColGrid());
}

void DawControlLook::DrawScrollBarBorder(BView* view, BRect rect, const BRect&,
        const rgb_color&, uint32, orientation) {
    StrokeRounded(view, rect, 0.0f, ColGrid());
}

void DawControlLook::DrawScrollBarButton(BView* view, BRect rect,
        const BRect& updateRect, const rgb_color& base, const rgb_color& text,
        uint32 flags, int32 direction, orientation orientation,
        bool pressed) {
    (void)text; (void)orientation;
    FillRounded(view, rect, 0.0f, pressed ? ColHeaderHi() : ColHeader());
    DrawArrow(view, rect, (uint32)direction,
              (flags & B_DISABLED) != 0 ? ColTextDim() : ColText());
    (void)updateRect; (void)base;
}

void DawControlLook::DrawScrollBarThumb(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, orientation,
        uint32 knobStyle) {
    (void)knobStyle;   // only one thumb style in this theme
    const rgb_color c = (flags & B_DISABLED) != 0 ? ColGrid() : ColHeaderHi();
    FillRounded(view, rect, Themed(2.0f), c);
}

void DawControlLook::DrawScrollViewFrame(BView* view, BRect& rect, const BRect&,
        BRect, BRect, const rgb_color&, border_style, uint32, uint32) {
    StrokeRounded(view, rect, 0.0f, ColGrid());
}

void DawControlLook::DrawArrowShape(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 direction, uint32 flags, float) {
    DrawArrow(view, rect, direction,
              (flags & B_DISABLED) != 0 ? ColTextDim() : ColText());
}

float DawControlLook::GetScrollBarWidth(orientation) {
    return Themed(14.0f);
}

// --- sliders -------------------------------------------------------------

rgb_color DawControlLook::SliderBarColor(const rgb_color&) { return ColLcd(); }

void DawControlLook::DrawSliderBar(BView* view, BRect rect, const BRect&,
        const rgb_color&, rgb_color leftFillColor, rgb_color rightFillColor,
        float sliderScale, uint32 flags, orientation orientation) {
    (void)leftFillColor; (void)rightFillColor; (void)sliderScale;
    DrawSliderBar(view, rect, BRect(), ColLcd(), ColLcd(), flags, orientation);
    (void)view;
}

void DawControlLook::DrawSliderBar(BView* view, BRect rect, const BRect&,
        const rgb_color&, rgb_color, uint32 flags, orientation orientation) {
    const float r = Themed(2.0f);
    FillRounded(view, rect, r, (flags & B_DISABLED) != 0 ? ColGrid() : ColLcd());
    (void)orientation;
}

void DawControlLook::DrawSliderThumb(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags, orientation) {
    FillRounded(view, rect, Themed(2.0f),
                (flags & B_DISABLED) != 0 ? ColGrid() : ColHeaderHi());
    StrokeRounded(view, rect, Themed(2.0f), ColBtnBorder());
}

void DawControlLook::DrawSliderTriangle(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags, orientation) {
    DrawSliderThumb(view, rect, rect, ColHeaderHi(), flags, B_HORIZONTAL);
}

void DawControlLook::DrawSliderTriangle(BView* view, BRect& rect, const BRect&,
        const rgb_color&, const rgb_color&, uint32 flags, orientation) {
    DrawSliderThumb(view, rect, rect, ColHeaderHi(), flags, B_HORIZONTAL);
}

void DawControlLook::DrawSliderHashMarks(BView* view, BRect& rect,
        const BRect&, const rgb_color&, int32 count, hash_mark_location,
        uint32, orientation orientation) {
    if (count < 2) return;
    view->SetHighColor(ColTextDim());
    for (int32 i = 0; i < count; i++) {
        const float t = (float)i / (float)(count - 1);
        if (orientation == B_HORIZONTAL) {
            const float x = rect.left + t * rect.Width();
            view->StrokeLine(BPoint(x, rect.top), BPoint(x, rect.bottom));
        } else {
            const float y = rect.bottom - t * rect.Height();
            view->StrokeLine(BPoint(rect.left, y), BPoint(rect.right, y));
        }
    }
}

// --- tabs, splitters, borders, group frames ------------------------------

void DawControlLook::DrawActiveTab(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags, uint32, uint32, int32, int32, int32,
        int32) {
    DrawFrame(view, rect, Themed(3.0f), ColHeader(), flags, ColBtnBorder());
}

void DawControlLook::DrawInactiveTab(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32 flags, uint32, uint32, int32, int32, int32,
        int32) {
    DrawFrame(view, rect, Themed(3.0f), ColBackground(), flags, ColGrid());
}

void DawControlLook::DrawTabFrame(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32, uint32, border_style, uint32) {
    StrokeRounded(view, rect, 0.0f, ColGrid());
}

void DawControlLook::DrawSplitter(BView* view, BRect& rect, const BRect&,
        const rgb_color&, orientation orientation, uint32 flags, uint32) {
    // The whole grab area is painted (the split view's own colour showed
    // through it otherwise), with a hairline down the middle; the line turns
    // the accent while it is being dragged. `orientation` is the SPLIT's, not
    // the bar's: a horizontal split lays its items side by side, so its bar
    // runs vertically.
    FillRounded(view, rect, 0.0f, ColGrid());
    const bool active = (flags & B_ACTIVATED) != 0;
    const float t = active ? Themed(2.0f) : Themed(1.0f);
    BRect line = rect;
    if (orientation == B_VERTICAL) {
        const float cy = floorf((rect.top + rect.bottom) * 0.5f);
        line.top = cy;
        line.bottom = cy + t - 1.0f;
    } else {
        const float cx = floorf((rect.left + rect.right) * 0.5f);
        line.left = cx;
        line.right = cx + t - 1.0f;
    }
    FillRounded(view, line, 0.0f, active ? ColAccent() : Rgb(52, 52, 60));
}

void DawControlLook::DrawBorder(BView* view, BRect& rect, const BRect&,
        const rgb_color&, border_style, uint32, uint32) {
    StrokeRounded(view, rect, 0.0f, ColBtnBorder());
}

void DawControlLook::DrawRaisedBorder(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32, uint32) {
    StrokeRounded(view, rect, 0.0f, ColHeaderHi());
}

void DawControlLook::DrawGroupFrame(BView* view, BRect& rect, const BRect&,
        const rgb_color&, uint32) {
    StrokeRounded(view, rect, Themed(3.0f), ColGrid());
}

void DawControlLook::DrawTextControlBorder(BView* view, BRect& rect,
        const BRect&, const rgb_color&, uint32 flags, uint32) {
    FillRounded(view, rect, Themed(2.0f), ColLcd());
    StrokeRounded(view, rect, Themed(2.0f),
                  (flags & B_FOCUSED) != 0 ? ColAccent() : ColBtnBorder());
}

// --- labels --------------------------------------------------------------

// Where a label goes in `rect`, honouring the caller's alignment: a stock
// BButton asks for centred, a check box or a menu field for left. Vertical
// placement uses the view's real font metrics, so the text sits in the middle
// of the box whatever the font size.
static BPoint LabelOrigin(BView* view, const char* label, const BRect& rect,
                          const BAlignment& align) {
    font_height fh;
    view->GetFontHeight(&fh);
    const float w = view->StringWidth(label);
    const float textH = ceilf(fh.ascent) + ceilf(fh.descent);
    float x = rect.left;
    if (align.horizontal == B_ALIGN_CENTER)
        x = floorf(rect.left + (rect.Width() - w) * 0.5f);
    else if (align.horizontal == B_ALIGN_RIGHT)
        x = rect.right - w;
    float y;
    if (align.vertical == B_ALIGN_TOP)
        y = rect.top + ceilf(fh.ascent);
    else if (align.vertical == B_ALIGN_BOTTOM)
        y = rect.bottom - ceilf(fh.descent);
    else
        y = floorf(rect.top + (rect.Height() - textH) * 0.5f + ceilf(fh.ascent));
    return BPoint(x, y);
}

void DawControlLook::DrawLabel(BView* view, const char* label, BRect rect,
        const BRect& updateRect, const rgb_color& base, uint32 flags,
        const rgb_color* textColor) {
    DrawLabel(view, label, rect, updateRect, base, flags,
              DefaultLabelAlignment(), textColor);
}

void DawControlLook::DrawLabel(BView* view, const char* label, BRect rect,
        const BRect&, const rgb_color&, uint32 flags,
        const BAlignment& alignment, const rgb_color*) {
    if (label == nullptr) return;
    view->SetHighColor((flags & B_DISABLED) != 0 ? ColTextDim() : ColText());
    view->DrawString(label, LabelOrigin(view, label, rect, alignment));
}

void DawControlLook::DrawLabel(BView* view, const char* label,
        const rgb_color&, uint32 flags, const BPoint& where,
        const rgb_color*) {
    if (label == nullptr) return;
    view->SetHighColor((flags & B_DISABLED) != 0 ? ColTextDim() : ColText());
    view->DrawString(label, where);
}

void DawControlLook::DrawLabel(BView* view, const char* label, const BBitmap*,
        BRect rect, const BRect& updateRect, const rgb_color& base,
        uint32 flags, const BAlignment& alignment, const rgb_color* textColor) {
    DrawLabel(view, label, rect, updateRect, base, flags, alignment, textColor);
}

} // namespace daw
