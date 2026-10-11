#include "ShortcutsWindow.h"

#include "../model/KeyMap.h"   // BuildShortcutsText: the one table
#include "Theme.h"

#include <LayoutBuilder.h>
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

    // The widest tab-expanded line decides the width.
    float textW = Themed(240.0f);   // a floor, so a short text still reads
    for (size_t i = 0; i < text.size(); ) {
        const size_t nl = text.find('\n', i);
        if (nl == std::string::npos) break;
        textW = std::max(textW, LineWidth(font, text.substr(i, nl - i), tabW));
        i = nl + 1;
    }

    // Fits its content, clamped to the screen (the guidelines' rule for a
    // content-sized window): past that the text view scrolls.
    BScreen screen;
    const BRect sf = screen.Frame();
    const float pad = Themed(8.0f);
    const float insetX = Themed(6.0f), insetY = Themed(4.0f);
    const float sbW = B_V_SCROLL_BAR_WIDTH + 1.0f;
    float w = std::min(textW + insetX * 2.0f + pad * 2.0f + sbW,
                       sf.Width() - Themed(40.0f));
    if (w < Themed(240.0f)) w = Themed(240.0f);

    const BRect inner(pad, pad, pad + w - sbW - pad * 2.0f,
                      pad + Themed(100.0f));
    BTextView* tv = new BTextView(inner, kShortcutsTextView,
                                  inner.OffsetToCopy(0.0f, 0.0f), &font, nullptr,
                                  B_FOLLOW_ALL, B_WILL_DRAW);
    tv->SetViewColor(ColLcd());
    tv->SetLowColor(ColLcd());
    tv->SetHighColor(ColText());
    tv->SetTabWidth(tabW);
    tv->SetInsets(insetX, insetY, insetX, insetY);
    tv->MakeEditable(false);
    tv->MakeSelectable(true);
    tv->SetDoesUndo(false);
    tv->SetWordWrap(false);
    tv->SetText(text.c_str());

    // The text view knows exactly how tall its text is at this width -- ask it
    // rather than estimating: an estimate a font's leading off clips the last
    // lines, which is the fault this replaced.
    float needMin = 0.0f, needMax = 0.0f, needH = 0.0f;
    tv->GetHeightForWidth(inner.Width(), &needMin, &needMax, &needH);
    const float contentH = std::max(needH, inner.Height());

    float h = std::min(contentH + pad * 2.0f + Themed(6.0f),
                       sf.Height() - Themed(60.0f));
    ResizeTo(w, h);
    SetSizeLimits(Themed(200.0f), sf.Width(), Themed(120.0f), sf.Height());

    BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f)
        .Add(new BScrollView("scroll", tv, B_FOLLOW_ALL, 0, false, true))
        .End();
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
