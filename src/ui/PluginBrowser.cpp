#include "PluginBrowser.h"

#include "UiMetrics.h"
#include "../plugin/PluginHost.h"

#ifdef DAW_HAVE_LV2
#include "../plugin/Lv2Host.h"
#endif

#include <ListView.h>
#include <ScrollView.h>
#include <StringItem.h>
#include <TextControl.h>

#include <algorithm>
#include <cctype>

namespace daw {

namespace {

enum {
    MSG_FILTER = 'pbfl',   // filter text changed
    MSG_PICK   = 'pbpk',   // list invoked (double-click / Enter)
};

std::string Lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

// The built-ins offered, matching the buttons the effects editor used to show.
// Biquad is deliberately absent: it is retained only so old projects still load
// (see EffectType in model/Effect.h), and the parametric EQ supersedes it for
// anything new.
struct BuiltIn { EffectType type; const char* label; };
const BuiltIn kBuiltIns[] = {
    { EffectType::Eq,         "EQ (5-band)" },
    { EffectType::Delay,      "Delay" },
    { EffectType::Reverb,     "Reverb" },
    { EffectType::Compressor, "Compressor" },
    { EffectType::Saturator,  "Saturator" },
    { EffectType::Gate,       "Gate" },
    { EffectType::Widener,    "Widener" },
    { EffectType::Limiter,    "Limiter" },
};

} // namespace

PluginBrowser::PluginBrowser(BRect frame, TrackId track, BMessenger target,
                             BMessenger main)
    : BWindow(frame, "Add Effect", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fTarget(target), fMain(main), fTrack(track) {
    BView* root = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    root->SetViewColor(ColHeader());
    AddChild(root);

    const float w = Bounds().Width();
    fFilter = new BTextControl(BRect(8, 8, w - 8, 30), "filter", "Find:", "",
                               new BMessage(MSG_FILTER));
    fFilter->SetDivider(36.0f);
    // Fire on every keystroke rather than only on Enter, so the list narrows as
    // the user types -- the point of a filter over a menu.
    fFilter->SetModificationMessage(new BMessage(MSG_FILTER));
    root->AddChild(fFilter);

    BRect lr(8, 38, w - 8 - B_V_SCROLL_BAR_WIDTH, Bounds().Height() - 8);
    fList = new BListView(lr, "list", B_SINGLE_SELECTION_LIST,
                          B_FOLLOW_ALL_SIDES);
    fList->SetInvocationMessage(new BMessage(MSG_PICK));
    root->AddChild(new BScrollView("sv", fList, B_FOLLOW_ALL_SIDES, 0,
                                   false, true));

    // Build the full catalogue once. The hosts scan at startup and their
    // listings do not change while the app runs, so re-reading them per
    // keystroke would buy nothing.
    for (const BuiltIn& b : kBuiltIns)
        fAll.push_back({ b.type, std::string(),
                         std::string("Built-in  -  ") + b.label,
                         Lower(b.label) });

    for (const PluginInfo& pi : PluginHost::Instance().Plugins())
        fAll.push_back({ EffectType::Plugin, pi.name,
                         "Add-on  -  " + pi.name, Lower(pi.name) });

#ifdef DAW_HAVE_LV2
    // Hostable plugins only -- the host filters out anything it could not
    // instantiate, so everything offered here will actually load.
    for (const Lv2PluginInfo& pi : Lv2Host::Instance().Plugins())
        fAll.push_back({ EffectType::Lv2, pi.uri,
                         "LV2  -  " + pi.name, Lower(pi.name) });
#endif

    Rebuild();
}

void PluginBrowser::Rebuild() {
    fList->MakeEmpty();
    fShown.clear();

    const std::string needle = Lower(fFilter ? fFilter->Text() : "");
    for (const Entry& e : fAll) {
        // Match the plugin's own name, not the decorated label, so typing "lv2"
        // does not match every LV2 plugin by way of its section prefix.
        if (!needle.empty() && e.search.find(needle) == std::string::npos)
            continue;
        fShown.push_back(e);
        fList->AddItem(new BStringItem(e.label.c_str()));
    }
    if (!fShown.empty()) fList->Select(0);   // Enter works without a click
}

void PluginBrowser::PostChoice(int32 index) {
    // Prefer the index carried by the invocation itself. The list can be
    // rebuilt between the double-click and this message arriving -- type a
    // character straight after picking and MSG_FILTER runs first, re-selecting
    // row 0 -- so reading the CURRENT selection here can insert a different
    // plugin from the one that was clicked.
    const int32 sel = index >= 0 ? index : fList->CurrentSelection();
    if (sel < 0 || sel >= (int32)fShown.size()) return;
    const Entry& e = fShown[(size_t)sel];

    BMessage m(kMsgPluginChosen);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt32("type", (int32)(int)e.type);
    m.AddString("name", e.id.c_str());
    fTarget.SendMessage(&m);
    PostMessage(B_QUIT_REQUESTED);   // one pick per opening, like a dialog
}

void PluginBrowser::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_FILTER: Rebuild(); break;
        case MSG_PICK: {
            // BListView puts the invoked row in "index"; absent, fall back.
            int32 idx = -1;
            if (msg->FindInt32("index", &idx) != B_OK) idx = -1;
            PostChoice(idx);
            break;
        }
        default: BWindow::MessageReceived(msg); break;
    }
}

void PluginBrowser::DispatchMessage(BMessage* m, BHandler* h) {
    // Space must reach the transport from any window. Not while the filter field
    // has focus, though -- there a space is a space.
    if (fFilter && fFilter->TextView() && !fFilter->TextView()->IsFocus())
        if (ForwardSpaceToTransport(m, fMain)) return;
    BWindow::DispatchMessage(m, h);
}

} // namespace daw
