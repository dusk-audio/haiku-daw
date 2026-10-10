// DawSegments — the kit's segmented control (T2): a row of labels, exactly one
// of them current, for switching a pane's page.
//
// Not a BTabView, and not several buttons either: a tab's label comes from its
// view's name (an unnamed view draws an empty tab), and a row of independent
// toggles lets the user "select" two at once, which a page switch cannot mean.
// The drawing is the control look's — stock Haiku in System mode,
// DawControlLook's in Dark mode — so a segment and a button are the same
// control in different states.
#pragma once

#include "DawControl.h"

#include <string>
#include <vector>

namespace daw {

class DawSegments : public BControl, public ThemeAware {
public:
    // `labels` is an array of `count` names; the message carries the chosen
    // index as int32 "index" (and BControl's own value).
    DawSegments(const char* name, const char* const* labels, int count,
                BMessage* message,
                uint32 flags = B_WILL_DRAW | B_NAVIGABLE);

    void ApplyTheme() override { AdoptPanelColors(this); }

    void SetSelected(int index);
    int  Selected() const { return fSelected; }

    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void KeyDown(const char* bytes, int32 numBytes) override;
    // Only the preferred size: overriding MinSize/MaxSize here would SHADOW
    // the explicit sizes the caller sets, and an uncapped segmented control
    // stretches across the whole strip and pushes its other controls off the
    // right edge (which is exactly what it did).
    void GetPreferredSize(float* width, float* height) override;

private:
    BRect SegmentRect(int index) const;
    int   SegmentAt(BPoint where) const;   // -1 when the point is in a gap

    std::vector<std::string> fLabels;
    int fSelected = 0;
};

} // namespace daw
