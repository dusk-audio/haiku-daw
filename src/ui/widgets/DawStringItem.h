// DawStringItem — a list row drawn in the THEME's text colour.
//
// BListView::DrawItem stamps the pen from ui_color(B_LIST_ITEM_TEXT_COLOR)
// before a row is drawn, and the app may not rewrite the system colour table
// (T1) — so a plain BStringItem draws black text on the theme's near-black
// well in Dark mode: the browsers' lists (a dock page now) were unreadable.
// The T2 review's follow-up found it in the dark-pass screenshots.
//
// Only an UNSELECTED row is overridden: a selected row keeps the system's
// selected-text colour, which belongs on the selection's own background.
// Haiku-only (Interface Kit); header-only, like the rest of the kit's pieces
// that only add a colour.
#pragma once

#include "../Theme.h"

#include <StringItem.h>
#include <View.h>

namespace daw {

class DawStringItem : public BStringItem {
public:
    explicit DawStringItem(const char* text) : BStringItem(text) {}

    void DrawItem(BView* owner, BRect frame, bool complete) override {
        if (owner == nullptr || IsSelected()) {
            BStringItem::DrawItem(owner, frame, complete);
            return;
        }
        // The pen BListView::DrawItem just set comes from the system colour
        // table; the row's own text is the theme's.
        const rgb_color pen = owner->HighColor();
        owner->SetHighColor(ColText());
        BStringItem::DrawItem(owner, frame, complete);
        owner->SetHighColor(pen);
    }
};

} // namespace daw
