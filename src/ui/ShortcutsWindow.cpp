#include "ShortcutsWindow.h"

#include "../model/KeyMap.h"   // BuildShortcutsText: the one table
#include "Theme.h"

#include <Screen.h>
#include <ScrollView.h>
#include <String.h>
#include <TextView.h>

#include <algorithm>
#include <string>

namespace daw {

namespace {

// One line of the generated text: the longest has a key name, a tab and a
// description, so the window has to be wide enough for the tab-expanded line.
float LineWidth(const BFont& font, const std::string& line, float tabW) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) return font.StringWidth(line.c_str());
    return font.StringWidth(line.substr(0, tab).c_str()) + tabW
         + font.StringWidth(line.substr(tab + 1).c_str());
}

} // namespace

ShortcutsWindow::ShortcutsWindow()
    : BWindow(BRect(0, 0, Themed(360.0f), Themed(420.0f)), "Keyboard Shortcuts",
              B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE) {
    const std::string text = BuildShortcutsText();
    const BFont font = ThemeFont();
    const float tabW = Themed(96.0f);

    font_height fi;
    font.GetHeight(&fi);
    const float lineH = fi.ascent + fi.descent + fi.leading;

    float textW = Themed(240.0f);   // a floor, so a short text still reads
    int lines = 0;
    for (size_t i = 0; i <= text.size(); ) {
        const size_t nl = text.find('\n', i);
        const std::string line = text.substr(
            i, nl == std::string::npos ? std::string::npos : nl - i);
        if (!line.empty() || nl != std::string::npos) {
            textW = std::max(textW, LineWidth(font, line, tabW));
            if (!line.empty()) lines++;
        }
        if (nl == std::string::npos) break;
        i = nl + 1;
    }

    // Fits its content, clamped to the screen (the guidelines' rule for a
    // content-sized window): past that the text view scrolls.
    BScreen screen;
    const BRect sf = screen.Frame();
    const float pad = Themed(8.0f);
    const float sbW = B_V_SCROLL_BAR_WIDTH + 1.0f;
    float w = std::min(textW + pad * 2.0f + sbW, sf.Width() - Themed(40.0f));
    float h = std::min(lines * lineH + pad * 2.0f + Themed(24.0f),
                       sf.Height() - Themed(60.0f));
    if (w < Themed(240.0f)) w = Themed(240.0f);
    ResizeTo(w, h);
    SetSizeLimits(Themed(200.0f), sf.Width(), Themed(120.0f), sf.Height());

    const BRect inner(pad, pad, pad + textW, pad + lines * lineH);
    BTextView* tv = new BTextView(inner, kShortcutsTextView,
                                  inner.OffsetToCopy(0.0f, 0.0f), &font, nullptr,
                                  B_FOLLOW_ALL, B_WILL_DRAW);
    tv->SetViewColor(ColLcd());
    tv->SetLowColor(ColLcd());
    tv->SetHighColor(ColText());
    tv->SetTabWidth(tabW);
    tv->SetInsets(Themed(6.0f), Themed(4.0f), Themed(6.0f), Themed(4.0f));
    tv->MakeEditable(false);
    tv->MakeSelectable(true);
    tv->SetDoesUndo(false);
    tv->SetWordWrap(false);
    tv->SetText(text.c_str());

    AddChild(new BScrollView("scroll", tv, B_FOLLOW_ALL, 0, false, true));
    CenterIn(sf);
}

void ShortcutsWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgShortcutsActivate) {   // already open: come forward
        Activate(true);
        SetWorkspaces(B_CURRENT_WORKSPACE);
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
