// DawSegments — see the header. Implemented here rather than in the header
// because it is the first kit control that needs more than draw-time tokens:
// the segments are laid out from the label widths.
#include "DawSegments.h"

#include <ControlLook.h>
#include <Window.h>

#include <cmath>

namespace daw {

DawSegments::DawSegments(const char* name, const char* const* labels, int count,
                         BMessage* message, uint32 flags)
    : BControl(name, nullptr, message, flags) {
    for (int i = 0; i < count; i++)
        fLabels.push_back(labels[i] != nullptr ? labels[i] : "");
    SetValue(0);
}

void DawSegments::SetSelected(int index) {
    if (index < 0 || index >= (int)fLabels.size() || index == fSelected) return;
    fSelected = index;
    SetValue(index);
    Invalidate();
}

BRect DawSegments::SegmentRect(int index) const {
    const BRect b = Bounds();
    const int n = (int)fLabels.size();
    if (n <= 0 || index < 0 || index >= n) return BRect();
    // Widths follow the labels, so "Editor" is wider than "×" — the whole row
    // is scaled to the bounds, which keeps the control's own size honest when
    // the font grows (M1.2).
    float total = 0.0f;
    std::vector<float> w((size_t)n);
    for (int i = 0; i < n; i++) {
        w[(size_t)i] = StringWidth(fLabels[(size_t)i].c_str()) + Themed(16.0f);
        total += w[(size_t)i];
    }
    if (total <= 0.0f) return BRect();
    const float scale = b.Width() / total;
    float x = b.left;
    for (int i = 0; i < index; i++) x += w[(size_t)i] * scale;
    BRect r(x, b.top, x + w[(size_t)index] * scale - Themed(1.0f), b.bottom);
    return r;
}

int DawSegments::SegmentAt(BPoint where) const {
    for (int i = 0; i < (int)fLabels.size(); i++) {
        if (SegmentRect(i).Contains(where)) return i;
    }
    return -1;
}

void DawSegments::Draw(BRect) {
    const rgb_color base = PanelColorOf(this);
    for (int i = 0; i < (int)fLabels.size(); i++) {
        BRect r = SegmentRect(i);
        if (!r.IsValid()) continue;
        uint32 flags = LookFlagsOf(IsEnabled(), false, IsFocus() && i == fSelected,
                                   false);
        if (i == fSelected) flags |= BControlLook::B_ACTIVATED;
        be_control_look->DrawButtonFrame(this, r, Bounds(), Themed(3.0f), base,
                                         base, flags, BControlLook::B_ALL_BORDERS);
        be_control_look->DrawButtonBackground(this, r, Bounds(), Themed(3.0f),
                                              base, flags,
                                              BControlLook::B_ALL_BORDERS,
                                              B_HORIZONTAL);
        be_control_look->DrawLabel(this, fLabels[(size_t)i].c_str(), r, Bounds(),
                                   base, flags,
                                   BAlignment(B_ALIGN_CENTER,
                                              B_ALIGN_VERTICAL_CENTER),
                                   nullptr);
    }
}

void DawSegments::MouseDown(BPoint where) {
    if (!IsEnabled()) return;
    const int hit = SegmentAt(where);
    if (hit < 0 || hit == fSelected) {
        MakeFocus(true);
        return;
    }
    MakeFocus(true);
    SetSelected(hit);
    Invoke();   // one click, one page: no drag, no release-to-commit
}

void DawSegments::KeyDown(const char* bytes, int32 numBytes) {
    // Left/right walk the pages, which is what a segmented control is for.
    if (numBytes == 1 && (bytes[0] == B_LEFT_ARROW || bytes[0] == B_RIGHT_ARROW)) {
        const int n = (int)fLabels.size();
        if (n > 0) {
            const int next = bytes[0] == B_LEFT_ARROW ? (fSelected + n - 1) % n
                                                      : (fSelected + 1) % n;
            SetSelected(next);
            Invoke();
        }
        return;
    }
    BControl::KeyDown(bytes, numBytes);
}

void DawSegments::GetPreferredSize(float* width, float* height) {
    float total = 0.0f;
    for (const std::string& l : fLabels)
        total += StringWidth(l.c_str()) + Themed(16.0f);
    if (width)  *width  = total + Themed(2.0f);
    if (height) *height = Themed(20.0f);
}

} // namespace daw
