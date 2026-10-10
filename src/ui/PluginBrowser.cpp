#include "PluginBrowser.h"

#include "UiMetrics.h"
#include "../plugin/PluginHost.h"

#ifdef DAW_HAVE_LV2
#include "../plugin/Lv2Host.h"
#endif

#include <LayoutBuilder.h>
#include <ListView.h>
#include <ScrollView.h>
#include <StringItem.h>
#include "widgets/DawTextField.h"   // the kit (M1.3)

#include <GroupLayout.h>
#include <LayoutBuilder.h>

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

void PluginBrowserView::ApplyTheme() {
    ApplyWellColors(fList);
    ApplyWellColors(fScroll);
}

PluginBrowserView::PluginBrowserView(TrackId track, BMessenger target)
    : BGroupView("pluginbrowserview", B_VERTICAL, 0.0f), fTarget(target),
      fTrack(track) {
    // The field and the list, laid out the way the window used to place them:
    // inside a real window the group's own layout does the work, inside the
    // dock the dock's. The window host hands this view the whole frame.
    SetViewColor(ColHeader());
    SetLowColor(ColHeader());
    BGroupLayout* g = GroupLayout();
    g->SetInsets(8.0f, 8.0f, 8.0f, 8.0f);
    g->SetSpacing(6.0f);

    fFilter = new DawTextField("filter", "Find:", "", new BMessage(MSG_FILTER));
    fFilter->SetDivider(36.0f);
    // Fire on every keystroke rather than only on Enter, so the list narrows as
    // the user types -- the point of a filter over a menu.
    fFilter->SetModificationMessage(new BMessage(MSG_FILTER));
    fFilter->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, Themed(24.0f)));
    g->AddView(fFilter);

    fList = new BListView("list", B_SINGLE_SELECTION_LIST);
    fList->SetInvocationMessage(new BMessage(MSG_PICK));
    fScroll = new BScrollView("sv", fList, 0, false, true);
    g->AddView(fScroll, 1.0f);

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

void PluginBrowserView::Rebuild() {
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

void PluginBrowserView::PostChoice(int32 index) {
    // Prefer the index carried by the invocation itself. The list can be
    // rebuilt between the double-click and this message arriving -- type a
    // character straight after picking and MSG_FILTER runs first, re-selecting
    // row 0 -- so reading the CURRENT selection here can insert a different
    // plugin from the one that was clicked.
    const int32 sel = index >= 0 ? index : fList->CurrentSelection();
    if (sel < 0 || sel >= (int32)fShown.size()) return;
    const Entry& e = fShown[(size_t)sel];

    // No track selected (the dock's page before the user picks one): the rows
    // are inert rather than inserting into whatever track happened to be first.
    if (fTrack == kInvalidTrackId)
        return;

    BMessage m(kMsgPluginChosen);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt32("type", (int32)(int)e.type);
    m.AddString("name", e.id.c_str());
    fTarget.SendMessage(&m);
    // One pick per opening when this view IS a window (the host quits on this);
    // docked, the page stays where the user put it.
    if (BWindow* w = Window())
        w->PostMessage(kMsgBrowserDone);
}

void PluginBrowserView::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_FILTER: Rebuild(); break;
        case MSG_PICK: {
            // BListView puts the invoked row in "index"; absent, fall back.
            int32 idx = -1;
            if (msg->FindInt32("index", &idx) != B_OK) idx = -1;
            PostChoice(idx);
            break;
        }
        default: BGroupView::MessageReceived(msg); break;
    }
}

// --- the standalone window -------------------------------------------------

PluginBrowser::PluginBrowser(BRect frame, TrackId track, BMessenger target,
                             BMessenger main)
    : BWindow(frame, "Add Effect", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fMain(main) {
    fView = new PluginBrowserView(track, target);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0.0f).Add(fView).End();
}

void PluginBrowser::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgBrowserDone) {   // one pick per opening (a dialog)
        PostMessage(B_QUIT_REQUESTED);
        return;
    }
    BWindow::MessageReceived(msg);
}

void PluginBrowser::DispatchMessage(BMessage* m, BHandler* h) {
    // Space must reach the transport from any window. Not while the filter field
    // has focus, though -- there a space is a space.
    BTextControl* filter = dynamic_cast<BTextControl*>(FindView("filter"));
    if (filter && filter->TextView() && !filter->TextView()->IsFocus())
        if (ForwardSpaceToTransport(m, fMain)) return;
    BWindow::DispatchMessage(m, h);
}

} // namespace daw
