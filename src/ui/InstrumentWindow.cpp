#include "InstrumentWindow.h"

#include "UiMetrics.h"

#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Slider.h>
#include <StringView.h>

#include <cstdio>
#include <utility>

namespace daw {

enum {
    MSG_WAVE = 'iwav',   // waveform chosen (field "wave")
    MSG_ADSR = 'iadr',   // an ADSR slider moved (fields "slot","min","max")
};

InstrumentWindow::InstrumentWindow(BRect frame, Instrument inst, TrackId track,
                                   BMessenger apply)
    : BWindow(frame, "Instrument", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fInst(inst), fTrack(track), fApply(apply) {
    fRoot = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    fRoot->SetViewColor(ColHeader());
    AddChild(fRoot);
    Build();
}

void InstrumentWindow::Apply() {
    BMessage m(kMsgApplyInstrument);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt32("wave", fInst.waveform);
    m.AddFloat("a", fInst.attack);
    m.AddFloat("d", fInst.decay);
    m.AddFloat("s", fInst.sustain);
    m.AddFloat("r", fInst.release);
    fApply.SendMessage(&m);
}

static BSlider* AdsrRow(BRect r, const char* label, int slot,
                        float mn, float mx, float value, BWindow* target) {
    BMessage* m = new BMessage(MSG_ADSR);
    m->AddInt32("slot", slot);
    m->AddFloat("min", mn);
    m->AddFloat("max", mx);
    BSlider* s = new BSlider(r, label, label, m, 0, 1000, B_HORIZONTAL);
    float t = (mx > mn) ? (value - mn) / (mx - mn) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    s->SetValue((int32)(t * 1000.0f));
    s->SetModificationMessage(new BMessage(*m));
    s->SetTarget(target);
    return s;
}

void InstrumentWindow::Build() {
    while (BView* c = fRoot->ChildAt(0)) { fRoot->RemoveChild(c); delete c; }
    const float w = Bounds().Width();
    float y = 8;

    // Waveform popup.
    BPopUpMenu* menu = new BPopUpMenu("wave");
    const char* names[4] = { "Sine", "Saw", "Square", "Triangle" };
    for (int i = 0; i < 4; i++) {
        BMessage* mm = new BMessage(MSG_WAVE);
        mm->AddInt32("wave", i);
        BMenuItem* it = new BMenuItem(names[i], mm);
        it->SetMarked(i == fInst.waveform);
        it->SetTarget(this);
        menu->AddItem(it);
    }
    BMenuField* field = new BMenuField(BRect(8, y, w - 8, y + 20),
        "wf", "Waveform:", menu);
    fRoot->AddChild(field);
    y += 30;

    auto add = [&](const char* lbl, int slot, float mn, float mx, float v) {
        fRoot->AddChild(AdsrRow(BRect(8, y, w - 8, y + 26), lbl, slot, mn, mx, v,
                                this));
        y += 32;
    };
    add("Attack (s)",  0, 0.0f, 1.0f, fInst.attack);
    add("Decay (s)",   1, 0.0f, 1.0f, fInst.decay);
    add("Sustain",     2, 0.0f, 1.0f, fInst.sustain);
    add("Release (s)", 3, 0.0f, 2.0f, fInst.release);
}

void InstrumentWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_WAVE: {
            int32 wv = 0;
            msg->FindInt32("wave", &wv);
            fInst.waveform = wv;
            Build();     // refresh the marked item
            Apply();
            break;
        }
        case MSG_ADSR: {
            int32 slot = 0, v = 0; float mn = 0, mx = 1;
            msg->FindInt32("slot", &slot);
            msg->FindFloat("min", &mn);
            msg->FindFloat("max", &mx);
            msg->FindInt32("be:value", &v);
            const float val = mn + (v / 1000.0f) * (mx - mn);
            switch (slot) {
                case 0: fInst.attack  = val; break;
                case 1: fInst.decay   = val; break;
                case 2: fInst.sustain = val; break;
                case 3: fInst.release = val; break;
            }
            Apply();     // value only; no rebuild
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
