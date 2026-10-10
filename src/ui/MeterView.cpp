#include "MeterView.h"

#include "UiMetrics.h"

#include <cmath>

namespace daw {

MeterView::MeterView(BRect frame)
    : BView(frame, "meter", B_FOLLOW_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW) {
    ApplyTheme();
}

void MeterView::ApplyTheme() {
    SetViewColor(ColHeader());
}

void MeterView::SetLevels(float l, float r) {
    // Ignore sub-pixel changes so a quiet signal doesn't repaint every tick.
    if (std::abs(l - fL) < 0.01f && std::abs(r - fR) < 0.01f)
        return;
    fL = l;
    fR = r;
    Invalidate();
}

void MeterView::Draw(BRect) {
    BRect b = Bounds();
    const float mid = (b.top + b.bottom) * 0.5f;
    DrawBar(BRect(b.left, b.top + 1, b.right, mid - 1), fL);
    DrawBar(BRect(b.left, mid + 1, b.right, b.bottom - 1), fR);
}

void MeterView::DrawBar(BRect r, float level) {
    SetHighColor(ColLane());
    FillRect(r);

    float lv = level; if (lv < 0) lv = 0; if (lv > 1) lv = 1;
    BRect fill = r;
    fill.right = r.left + r.Width() * lv;
    // The theme's one threshold set (M1.2): this used to disagree with
    // Theme.h's MeterColor (1.0/0.7 here, 0.9/0.6 there).
    SetHighColor(MeterColor(level));
    FillRect(fill);

    SetHighColor(ColGrid());
    StrokeRect(r);
}

} // namespace daw
