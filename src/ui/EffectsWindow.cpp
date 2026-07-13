#include "EffectsWindow.h"

#include "UiMetrics.h"

#include <Button.h>
#include <Slider.h>
#include <StringView.h>

namespace daw {

enum {
    MSG_EP   = 'epar',   // param slider changed
    MSG_ERM  = 'erm ',   // remove effect
    MSG_EADD = 'eadd',   // add effect (field "kind": 0 LP, 1 HP, 2 Delay)
};

static void SetSlot(EffectDesc& d, int slot, float v) {
    switch (slot) { case 0: d.p0=v; break; case 1: d.p1=v; break;
                    case 2: d.p2=v; break; case 3: d.p3=v; break; }
}
static float GetSlot(const EffectDesc& d, int slot) {
    switch (slot) { case 0: return d.p0; case 1: return d.p1;
                    case 2: return d.p2; case 3: return d.p3; }
    return 0.0f;
}

EffectsWindow::EffectsWindow(BRect frame, std::vector<EffectDesc> chain,
                             TrackId track, BMessenger apply)
    : BWindow(frame, "Effects", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fChain(std::move(chain)), fTrack(track), fApply(apply) {
    fRoot = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    fRoot->SetViewColor(ColHeader());
    AddChild(fRoot);
    Rebuild();
}

void EffectsWindow::Apply() {
    BMessage m(kMsgApplyFx);
    m.AddInt64("track", (int64)fTrack);
    for (const EffectDesc& d : fChain) {
        m.AddInt32("et", (int32)(d.type == EffectType::Delay ? 1 : 0));
        m.AddFloat("e0", d.p0);
        m.AddFloat("e1", d.p1);
        m.AddFloat("e2", d.p2);
        m.AddFloat("e3", d.p3);
    }
    fApply.SendMessage(&m);
}

static BSlider* ParamRow(BRect r, const char* label, int fxIndex, int slot,
                         float min, float max, float value, BWindow* target) {
    BMessage* m = new BMessage(MSG_EP);
    m->AddInt32("fx", fxIndex);
    m->AddInt32("slot", slot);
    m->AddFloat("min", min);
    m->AddFloat("max", max);
    BSlider* s = new BSlider(r, label, label, m, 0, 1000, B_HORIZONTAL);
    float t = (max > min) ? (value - min) / (max - min) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    s->SetValue((int32)(t * 1000.0f));
    s->SetModificationMessage(new BMessage(*m));   // live while dragging
    s->SetTarget(target);
    return s;
}

void EffectsWindow::Rebuild() {
    while (BView* c = fRoot->ChildAt(0)) { fRoot->RemoveChild(c); delete c; }

    float y = 8.0f;
    const float w = Bounds().Width();

    for (size_t i = 0; i < fChain.size(); i++) {
        const EffectDesc& d = fChain[i];
        const bool isDelay = (d.type == EffectType::Delay);

        BStringView* title = new BStringView(BRect(8, y, w - 74, y + 16),
            "title", isDelay ? "Delay" : "Biquad filter");
        title->SetViewColor(ColHeader());
        title->SetHighColor(ColText());
        fRoot->AddChild(title);

        BButton* rm = new BButton(BRect(w - 70, y - 2, w - 8, y + 20),
            "rm", "Remove", new BMessage(MSG_ERM));
        rm->Message()->AddInt32("fx", (int32)i);
        fRoot->AddChild(rm);
        y += 24;

        auto add = [&](const char* lbl, int slot, float mn, float mx) {
            fRoot->AddChild(ParamRow(BRect(8, y, w - 8, y + 24), lbl,
                                     (int)i, slot, mn, mx, GetSlot(d, slot), this));
            y += 30;
        };
        if (isDelay) {
            add("Time", 0, 0.01f, 1.0f);
            add("Feedback", 1, 0.0f, 0.95f);
            add("Mix", 2, 0.0f, 1.0f);
        } else {
            add("Freq", 1, 20.0f, 16000.0f);
            add("Q", 2, 0.1f, 10.0f);
            if (d.p0 == 2.0f) add("Gain dB", 3, -24.0f, 24.0f);
        }
        y += 8;
    }

    const char* names[3] = { "Add Low-pass", "Add High-pass", "Add Delay" };
    for (int k = 0; k < 3; k++) {
        BButton* b = new BButton(BRect(8, y, w - 8, y + 22), "add",
                                 names[k], new BMessage(MSG_EADD));
        b->Message()->AddInt32("kind", k);
        fRoot->AddChild(b);
        y += 26;
    }
}

void EffectsWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_EP: {
            int32 fx = -1, slot = 0; float mn = 0, mx = 1;
            msg->FindInt32("fx", &fx);
            msg->FindInt32("slot", &slot);
            msg->FindFloat("min", &mn);
            msg->FindFloat("max", &mx);
            int32 v = 0;
            msg->FindInt32("be:value", &v);
            if (fx >= 0 && (size_t)fx < fChain.size()) {
                SetSlot(fChain[(size_t)fx], (int)slot,
                        mn + (v / 1000.0f) * (mx - mn));
                Apply();   // no Rebuild: only the value changed
            }
            break;
        }
        case MSG_ERM: {
            int32 fx = -1;
            msg->FindInt32("fx", &fx);
            if (fx >= 0 && (size_t)fx < fChain.size()) {
                fChain.erase(fChain.begin() + fx);
                Rebuild();
                Apply();
            }
            break;
        }
        case MSG_EADD: {
            int32 kind = 0;
            msg->FindInt32("kind", &kind);
            if      (kind == 0) fChain.push_back(LowPassDesc(800.0f));
            else if (kind == 1) fChain.push_back(HighPassDesc(200.0f));
            else                fChain.push_back(DelayDesc());
            Rebuild();
            Apply();
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
