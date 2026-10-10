#include "SendsWindow.h"

#include "UiMetrics.h"
#include "widgets/DawButton.h"      // the kit (M1.3)
#include "widgets/DawCheckBox.h"
#include "widgets/DawMenuField.h"
#include "widgets/DawSlider.h"

#include <MenuItem.h>
#include <PopUpMenu.h>
#include <StringView.h>

#include <cstdio>
#include <utility>

namespace daw {

enum {
    MSG_SLVL  = 'slvl',   // level slider changed  (field "send")
    MSG_SDEST = 'sdst',   // destination chosen    (fields "send", "dest")
    MSG_SPRE  = 'spre',   // pre/post toggled       (field "send")
    MSG_SRM   = 'srm ',   // remove send            (field "send")
    MSG_SADD  = 'sadd',   // add a send
};

static constexpr float kLevelMax = 2.0f;   // sends can exceed unity

SendsWindow::SendsWindow(BRect frame, std::vector<Send> sends,
                         std::vector<std::pair<TrackId, std::string>> buses,
                         TrackId track, BMessenger apply)
    : BWindow(frame, "Sends", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fSends(std::move(sends)), fBuses(std::move(buses)),
      fTrack(track), fApply(apply) {
    fRoot = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    fRoot->SetViewColor(ColHeader());
    AddChild(fRoot);
    Rebuild();
}

void SendsWindow::Apply() {
    BMessage m(kMsgApplySends);
    m.AddInt64("track", (int64)fTrack);
    for (const Send& s : fSends) {
        m.AddInt64("sd", (int64)s.dest);
        m.AddFloat("sl", s.level);
        m.AddInt32("sp", s.preFader ? 1 : 0);
    }
    fApply.SendMessage(&m);
}

// Name of a bus by id (for the dest menu label). "?" if it vanished.
static const char* BusName(const std::vector<std::pair<TrackId, std::string>>& b,
                           TrackId id) {
    for (const auto& p : b)
        if (p.first == id) return p.second.c_str();
    return "?";
}

void SendsWindow::Rebuild() {
    while (BView* c = fRoot->ChildAt(0)) { fRoot->RemoveChild(c); delete c; }

    // Design pixels through Themed(): the window is sized by its caller.
    const float pad  = Themed(8.0f);
    const float rowH = Themed(26.0f);
    float y = Themed(8.0f);
    const float w = Bounds().Width();

    if (fBuses.empty()) {
        BStringView* hint = new BStringView(BRect(pad, y, w - pad,
                                                  y + Themed(18.0f)),
            "hint", "No bus tracks. Create a Bus (Track > New Bus) to send to.");
        hint->SetViewColor(ColHeader());
        hint->SetHighColor(ColText());
        fRoot->AddChild(hint);
        return;
    }

    for (size_t i = 0; i < fSends.size(); i++) {
        const Send& s = fSends[i];

        // Destination popup.
        BPopUpMenu* menu = new BPopUpMenu(BusName(fBuses, s.dest));
        for (const auto& p : fBuses) {
            BMessage* mm = new BMessage(MSG_SDEST);
            mm->AddInt32("send", (int32)i);
            mm->AddInt64("dest", (int64)p.first);
            BMenuItem* it = new BMenuItem(p.second.c_str(), mm);
            it->SetMarked(p.first == s.dest);
            it->SetTarget(this);   // items route to this window (menu has no SetTarget)
            menu->AddItem(it);
        }
        DawMenuField* field = new DawMenuField(BRect(pad, y,
                                                     w - Themed(210.0f),
                                                     y + Themed(20.0f)),
            "dest", "To:", menu);
        fRoot->AddChild(field);

        DawCheckBox* pre = new DawCheckBox(BRect(w - Themed(200.0f), y,
                                                 w - Themed(96.0f),
                                                 y + Themed(20.0f)),
            "pre", "Pre-fader", new BMessage(MSG_SPRE));
        pre->Message()->AddInt32("send", (int32)i);
        pre->SetValue(s.preFader ? B_CONTROL_ON : B_CONTROL_OFF);
        pre->SetTarget(this);
        fRoot->AddChild(pre);

        DawButton* rm = new DawButton(BRect(w - Themed(90.0f), y - Themed(2.0f),
                                            w - pad, y + Themed(20.0f)),
            "rm", "Remove", new BMessage(MSG_SRM));
        rm->Message()->AddInt32("send", (int32)i);
        fRoot->AddChild(rm);
        y += rowH;

        // Level slider.
        BMessage* lm = new BMessage(MSG_SLVL);
        lm->AddInt32("send", (int32)i);
        DawSlider* sl = new DawSlider(BRect(pad, y, w - pad,
                                            y + Themed(26.0f)),
            "level", "Level", lm, 0, 1000, B_HORIZONTAL);
        float t = s.level / kLevelMax;
        if (t < 0) t = 0; if (t > 1) t = 1;
        sl->SetValue((int32)(t * 1000.0f));
        sl->SetModificationMessage(new BMessage(*lm));
        sl->SetTarget(this);
        fRoot->AddChild(sl);
        y += Themed(34.0f);
    }

    DawButton* add = new DawButton(BRect(pad, y, w - pad, y + Themed(22.0f)),
                                   "add", "Add Send", new BMessage(MSG_SADD));
    fRoot->AddChild(add);
}

void SendsWindow::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardSpaceToTransport(m, fApply)) return;
    BWindow::DispatchMessage(m, h);
}

void SendsWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_SLVL: {
            int32 send = -1, v = 0;
            msg->FindInt32("send", &send);
            msg->FindInt32("be:value", &v);
            if (send >= 0 && (size_t)send < fSends.size()) {
                fSends[(size_t)send].level = (v / 1000.0f) * kLevelMax;
                Apply();   // value only, no Rebuild
            }
            break;
        }
        case MSG_SDEST: {
            int32 send = -1; int64 dest = 0;
            msg->FindInt32("send", &send);
            msg->FindInt64("dest", &dest);
            if (send >= 0 && (size_t)send < fSends.size()) {
                fSends[(size_t)send].dest = (TrackId)dest;
                Rebuild();   // refresh the menu label + marks
                Apply();
            }
            break;
        }
        case MSG_SPRE: {
            int32 send = -1;
            msg->FindInt32("send", &send);
            if (send >= 0 && (size_t)send < fSends.size()) {
                fSends[(size_t)send].preFader = !fSends[(size_t)send].preFader;
                Apply();
            }
            break;
        }
        case MSG_SRM: {
            int32 send = -1;
            msg->FindInt32("send", &send);
            if (send >= 0 && (size_t)send < fSends.size()) {
                fSends.erase(fSends.begin() + send);
                Rebuild();
                Apply();
            }
            break;
        }
        case MSG_SADD: {
            if (!fBuses.empty()) {
                Send s;
                s.dest = fBuses.front().first;
                s.level = 1.0f;
                s.preFader = false;
                fSends.push_back(s);
                Rebuild();
                Apply();
            }
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
