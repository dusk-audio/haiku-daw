// DawMenuField — the kit's labelled pop-up field (M1.3).
//
// A BMenuField subclass. The menu itself, the click that opens it, the marked
// item and the field's label all stay stock; the field's frame, well and pop-up
// indicator come from the control look (T1), so the field matches the stock
// Haiku one in System mode and the dark panel in Dark mode.
#pragma once

#include "DawControl.h"   // AdoptPanelColors + the look's flag helpers

#include "../Theme.h"   // ThemedMenuItem (the popup's own rows)
#include <MenuBar.h>
#include <PopUpMenu.h>   // Go(): the popup every field in this app is built from
#include <MenuField.h>
#include <MenuItem.h>   // BMenuItem::Label (the marked item)
#include <Window.h>     // the popup this view opens

namespace daw {

class DawMenuField : public BMenuField, public ThemeAware {
public:
    void ApplyTheme() override {
        AdoptPanelColors(this);
        HideStockBar();
    }

    DawMenuField(BRect frame, const char* name, const char* label, BMenu* menu,
                 uint32 resizingMode = B_FOLLOW_LEFT_TOP,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(frame, name, label, menu, resizingMode, flags) {
        HideStockBar();
    }

    DawMenuField(const char* name, const char* label, BMenu* menu,
                 uint32 flags = B_WILL_DRAW | B_NAVIGABLE)
        : BMenuField(name, label, menu, flags) {
        HideStockBar();
    }

    void AttachedToWindow() override {
        BMenuField::AttachedToWindow();
        AdoptPanelColors(this);
        HideStockBar();
        SetHighColor(ColText());
        if (Label() != nullptr && Label()[0] != '\0' && ToolTip() == nullptr)
            SetToolTip(Label());
    }

    // The click opens the menu from the VIEW, because the view is what draws
    // the field now (see HideStockBar). A DISABLED field opens nothing: the
    // stock field's guard was the hidden bar's enabled state, and the bar is
    // never asked here -- the placeholder row (Preferences' output device) is
    // the one that made this visible.
    void MouseDown(BPoint) override {
        if (IsEnabled()) OpenMenu();
    }

    void KeyDown(const char* bytes, int32 numBytes) override {
        if (IsEnabled()) {
            for (int32 i = 0; i < numBytes; i++)
                if (bytes[i] == B_SPACE || bytes[i] == B_RETURN
                    || bytes[i] == B_DOWN_ARROW) {
                    OpenMenu();
                    return;
                }
        }
        BMenuField::KeyDown(bytes, numBytes);
    }

    // The field is drawn through the control look (T1): a stock frame and
    // pop-up indicator in System mode, DawControlLook's recessed well in Dark
    // mode, with the marked item's text and the label placed by the look.
    void Draw(BRect) override {
        const BRect b = Bounds();
        const rgb_color base = PanelColorOf(this);
        const uint32 flags = LookFlagsOf(IsEnabled(), false, IsFocus(), false);
        const char* label = Label();
        float left = b.left;
        if (label != nullptr && label[0] != '\0') {
            // The field's own label ("Output:", "Send to:") in the dim text
            // colour, which is the token that means "a label, not a value".
            SetHighColor(ColTextDim());
            DrawString(label, BPoint(b.left + Themed(1.0f),
                                     b.top + (b.Height()
                                              + ThemeFontSize() * 0.8f) * 0.5f));
            left += StringWidth(label) + Themed(6.0f);
        }

        BRect well(left, b.top, b.right, b.bottom);
        be_control_look->DrawMenuFieldFrame(this, well, b, base, base, flags,
                                            BControlLook::B_ALL_BORDERS);
        be_control_look->DrawMenuFieldBackground(this, well, b, base, true,
                                                 flags);

        const char* text = "";
        if (Menu() != nullptr) {
            if (BMenuItem* marked = Menu()->FindMarked())
                text = marked->Label();
        }
        SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        const float baseline = b.top + (b.Height() + ThemeFontSize() * 0.8f) * 0.5f;
        DrawString(text, BPoint(well.left + Themed(5.0f), baseline));
    }

    void GetPreferredSize(float* width, float* height) override {
        if (width) {
            const char* label = Label();
            *width = (label != nullptr ? StringWidth(label) + Themed(6.0f) : 0.0f)
                   + Themed(110.0f);
        }
        if (height) *height = Themed(20.0f);
    }

private:
    // The stock BMenuField draws the marked item's label ITSELF, through its
    // inner BMenuBar, and a BMenuItem's text colour comes from the system
    // colour table (`ui_color(B_MENU_ITEM_TEXT_COLOR)`) — which this app may
    // not write (T1). In Dark mode that is black text over the app's own dark
    // well: unreadable in every menu field in Preferences, Project Settings
    // and New Project (found by the M3.5 screenshot pass). So the inner bar is
    // hidden and the field draws its own value (Draw, above); the click and the
    // key that used to reach the bar come here instead.
    void HideStockBar() {
        if (BMenuBar* bar = MenuBar()) {
            if (!bar->IsHidden()) bar->Hide();
        }
    }

    void OpenMenu() {
        BMenu* menu = Menu();
        if (menu == nullptr) return;

        // A FRESH, unparented pop-up, built from this menu's rows -- never the
        // field's own menu, which is attached to the field's inner (hidden)
        // BMenuBar. BPopUpMenu::Go() runs the tracking on its own thread, and
        // the attached path has that thread touch views owned by this window's
        // looper: picking an item stops the app in the debugger ("Looper must
        // be locked"), which is what a user reported as "selecting a different
        // option crashes". Every other pop-up in this app is standalone
        // (InspectorView's input picker, the clip/lane/grid menus), which is
        // exactly why they are fine. The pick is mapped back by index.
        BPopUpMenu* popup = new BPopUpMenu("pick", false, false);
        for (int32 i = 0; i < menu->CountItems(); i++) {
            if (BMenuItem* src = menu->ItemAt(i)) {
                BMenuItem* it = new ThemedMenuItem(src->Label(), nullptr);
                it->SetMarked(src->IsMarked());
                it->SetEnabled(src->IsEnabled());
                popup->AddItem(it);
            } else {
                popup->AddItem(new BSeparatorItem());
            }
        }
        const BPoint at = ConvertToScreen(BPoint(0.0f, Bounds().bottom + 1.0f));
        BMenuItem* chosen = popup->Go(at, false, true);
        int32 picked = -1;
        for (int32 i = 0; chosen != nullptr && i < popup->CountItems(); i++)
            if (popup->ItemAt(i) == chosen) { picked = i; break; }
        delete popup;
        if (picked < 0) return;

        // What the stock field does with a choice: exactly one marked row, and
        // the row's own message delivered to its own target (the window, for
        // every field in this app). The setting rows carry no message at all --
        // the Preferences and Project Settings windows read their controls on
        // Apply -- so for them this is the marking and nothing else.
        for (int32 i = 0; i < menu->CountItems(); i++)
            if (BMenuItem* it = menu->ItemAt(i)) it->SetMarked(i == picked);
        if (BMenuItem* real = menu->ItemAt(picked)) {
            if (BMessage* msg = real->Message()) {
                BMessage send(*msg);
                BLooper* looper = nullptr;
                BHandler* handler = real->Target(&looper);
                BMessenger target(handler, looper);
                if (target.IsValid())         target.SendMessage(&send);
                else if (Window() != nullptr) Window()->PostMessage(&send);
            }
        }
        Invalidate();
    }
};

} // namespace daw
