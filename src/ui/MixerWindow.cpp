#include "MixerWindow.h"

#include "UiMetrics.h"

#include <Message.h>

namespace daw {

enum {
    MSG_MX_GAIN = 'mxgn',   // gain slider moved   (int32 "idx")
    MSG_MX_PAN  = 'mxpn',   // pan slider moved     (int32 "idx")
    MSG_MX_MUTE = 'mxmu',   // "M" toggled          (int32 "idx")
    MSG_MX_SOLO = 'mxso',   // "S" toggled          (int32 "idx")
};

// Per-strip geometry (pixels, in the root view's coordinate space).
static const float kMxStripW    = 84.0f;
static const float kMxMargin    = 8.0f;
static const float kMxNameTop   = 6.0f;
static const float kMxGainTop   = 28.0f;
static const float kMxGainH     = 150.0f;
static const float kMxPanTop    = kMxGainTop + kMxGainH + 8.0f;   // 186
static const float kMxPanH      = 28.0f;
static const float kMxBtnTop    = kMxPanTop + kMxPanH + 6.0f;     // 220
static const float kMxBtnH      = 22.0f;

MixerWindow::MixerWindow(BRect frame, std::vector<MixerStripInfo> strips,
                         BMessenger apply)
    : BWindow(frame, "Mixer", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fApply(apply) {
    fStrips.reserve(strips.size());
    for (MixerStripInfo& s : strips) {
        Strip st;
        st.info = std::move(s);
        fStrips.push_back(std::move(st));
    }

    fRoot = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    fRoot->SetViewColor(ColHeader());
    AddChild(fRoot);
    Build();
}

void MixerWindow::Build() {
    for (size_t i = 0; i < fStrips.size(); i++) {
        Strip& st = fStrips[i];
        const MixerStripInfo& in = st.info;
        const float x0 = kMxMargin + (float)i * kMxStripW;

        // Track name.
        BStringView* name = new BStringView(
            BRect(x0 + 4, kMxNameTop, x0 + kMxStripW - 4, kMxNameTop + 16),
            "name", in.name.c_str());
        name->SetViewColor(ColHeader());
        name->SetHighColor(ColText());
        fRoot->AddChild(name);

        // Vertical gain slider: 0..150 maps to gain 0..1.5 (value = gain*100).
        BMessage* gm = new BMessage(MSG_MX_GAIN);
        gm->AddInt32("idx", (int32)i);
        st.gain = new BSlider(
            BRect(x0 + kMxStripW / 2 - 16, kMxGainTop,
                  x0 + kMxStripW / 2 + 16, kMxGainTop + kMxGainH),
            "gain", "Gain", gm, 0, 150, B_VERTICAL);
        st.gain->SetValue((int32)(in.gain * 100.0f));
        st.gain->SetModificationMessage(new BMessage(MSG_MX_GAIN));
        st.gain->ModificationMessage()->AddInt32("idx", (int32)i);  // live drag
        st.gain->SetTarget(this);
        fRoot->AddChild(st.gain);

        // Horizontal pan slider: -100..100 maps to pan -1..+1 (value = pan*100).
        BMessage* pm = new BMessage(MSG_MX_PAN);
        pm->AddInt32("idx", (int32)i);
        st.pan = new BSlider(
            BRect(x0 + 6, kMxPanTop, x0 + kMxStripW - 6, kMxPanTop + kMxPanH),
            "pan", "Pan", pm, -100, 100, B_HORIZONTAL);
        st.pan->SetValue((int32)(in.pan * 100.0f));
        st.pan->SetModificationMessage(new BMessage(MSG_MX_PAN));
        st.pan->ModificationMessage()->AddInt32("idx", (int32)i);
        st.pan->SetTarget(this);
        fRoot->AddChild(st.pan);

        // Mute / Solo toggle buttons.
        BMessage* mm = new BMessage(MSG_MX_MUTE);
        mm->AddInt32("idx", (int32)i);
        st.mute = new BButton(
            BRect(x0 + 6, kMxBtnTop, x0 + kMxStripW / 2 - 2, kMxBtnTop + kMxBtnH),
            "mute", "M", mm);
        st.mute->SetValue(in.muted ? B_CONTROL_ON : B_CONTROL_OFF);
        st.mute->SetTarget(this);
        fRoot->AddChild(st.mute);

        BMessage* sm = new BMessage(MSG_MX_SOLO);
        sm->AddInt32("idx", (int32)i);
        st.solo = new BButton(
            BRect(x0 + kMxStripW / 2 + 2, kMxBtnTop, x0 + kMxStripW - 6,
                  kMxBtnTop + kMxBtnH),
            "solo", "S", sm);
        st.solo->SetValue(in.soloed ? B_CONTROL_ON : B_CONTROL_OFF);
        st.solo->SetTarget(this);
        fRoot->AddChild(st.solo);
    }
}

void MixerWindow::Apply(size_t i) {
    if (i >= fStrips.size()) return;
    const MixerStripInfo& in = fStrips[i].info;
    BMessage m(kMsgApplyMix);
    m.AddInt64("track", (int64)in.trackId);
    m.AddFloat("gain", in.gain);
    m.AddFloat("pan", in.pan);
    m.AddBool("mute", in.muted);
    m.AddBool("solo", in.soloed);
    fApply.SendMessage(&m);
}

void MixerWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_MX_GAIN: {
            int32 idx = -1;
            msg->FindInt32("idx", &idx);
            if (idx >= 0 && (size_t)idx < fStrips.size() && fStrips[idx].gain) {
                fStrips[idx].info.gain = fStrips[idx].gain->Value() / 100.0f;
                Apply((size_t)idx);
            }
            break;
        }
        case MSG_MX_PAN: {
            int32 idx = -1;
            msg->FindInt32("idx", &idx);
            if (idx >= 0 && (size_t)idx < fStrips.size() && fStrips[idx].pan) {
                fStrips[idx].info.pan = fStrips[idx].pan->Value() / 100.0f;
                Apply((size_t)idx);
            }
            break;
        }
        case MSG_MX_MUTE: {
            int32 idx = -1;
            msg->FindInt32("idx", &idx);
            if (idx >= 0 && (size_t)idx < fStrips.size()) {
                bool& m = fStrips[idx].info.muted;
                m = !m;
                if (fStrips[idx].mute)
                    fStrips[idx].mute->SetValue(m ? B_CONTROL_ON : B_CONTROL_OFF);
                Apply((size_t)idx);
            }
            break;
        }
        case MSG_MX_SOLO: {
            int32 idx = -1;
            msg->FindInt32("idx", &idx);
            if (idx >= 0 && (size_t)idx < fStrips.size()) {
                bool& s = fStrips[idx].info.soloed;
                s = !s;
                if (fStrips[idx].solo)
                    fStrips[idx].solo->SetValue(s ? B_CONTROL_ON : B_CONTROL_OFF);
                Apply((size_t)idx);
            }
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
