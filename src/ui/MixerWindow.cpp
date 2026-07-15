#include "MixerWindow.h"

#include "UiMetrics.h"
#include "Widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace daw {

// Strip geometry (Logic-style vertical channel strip).
static constexpr float kStripW  = 96.0f;
static constexpr float kGap     = 4.0f;
static constexpr float kPadT    = 6.0f;
static constexpr float kMaxGain = 1.5f;

// All the sub-control rects of one strip, laid out top -> bottom.
struct StripLayout {
    BRect input, fx, sends, out, autob, inst;   // section buttons
    BRect pan, val, fader, meter;
    BRect mon, arm, mute, solo, name;
};

static StripLayout LayoutStrip(float x0, float h) {
    StripLayout s;
    const float L = x0 + 6, R = x0 + kStripW - 6;
    const float cx = x0 + kStripW * 0.5f;
    float y = kPadT + 4;
    auto row = [&](float ht) { BRect r(L, y, R, y + ht); y += ht + 3; return r; };
    s.input = row(15);
    s.fx    = row(15);
    s.sends = row(15);
    s.out   = row(15);
    s.autob = row(15);
    s.inst  = row(15);            // MIDI only
    y += 4;
    s.pan = BRect(cx - 21, y, cx + 21, y + 42); y += 46;
    s.val = BRect(L, y, R, y + 14); y += 16;
    // Reserve room for I/R, M/S, name; never let the fader invert / zero-out
    // (guards a div-by-zero in the fader drag on a very short window).
    const float botTop = std::max(h - 76.0f, y + 20.0f);
    s.fader = BRect(cx - 30, y, cx - 4, botTop);
    s.meter = BRect(cx + 6,  y, cx + 26, botTop);
    float by = h - 72;
    s.mon  = BRect(L, by, cx - 2, by + 18);
    s.arm  = BRect(cx + 2, by, R, by + 18); by += 22;
    s.mute = BRect(L, by, cx - 2, by + 18);
    s.solo = BRect(cx + 2, by, R, by + 18);
    s.name = BRect(x0, h - 24, x0 + kStripW, h - 6);
    return s;
}

MixerStripsView::MixerStripsView(BRect frame, std::vector<MixerStripInfo> strips,
                                 float masterGain, BMessenger apply)
    : BView(frame, "strips", B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
      fStrips(std::move(strips)), fMasterGain(masterGain), fApply(apply) {
    SetViewColor(ColBackground());
}

float MixerStripsView::StripX(int i) const {
    return 8.0f + i * (kStripW + kGap);
}

void MixerStripsView::SetPeaks(
        const std::map<uint64, std::pair<float, float>>& peaks,
        float masterL, float masterR) {
    fPeaks = peaks;
    fMasterL = masterL; fMasterR = masterR;
    Invalidate();
}

void MixerStripsView::DrawStrip(int i, const MixerStripInfo* info, float gain,
                                float peakL, float peakR, bool master) {
    const float x0 = StripX(i);
    const float h  = Bounds().Height();
    BRect strip(x0, 2, x0 + kStripW, h - 4);
    const StripLayout L = LayoutStrip(x0, h);
    const int  type = info ? info->type : 0;
    const rgb_color accent = master ? Rgb(200, 165, 90)
                           : type == 1 ? ColMidiAccent() : ColAudioAccent();
    const rgb_color tc = master ? Rgb(150, 120, 70)
                       : (info ? TrackColor(info->colorIndex) : ColAccent());

    SetHighColor(ColHeader());
    FillRect(strip);

    // Section buttons (track strips only).
    if (info) {
        char b[24];
        DrawButton(this, L.input, info->hasInput ? "In" : "\xE2\x80\x94",
                   info->hasInput, Rgb(52, 104, 74));
        std::snprintf(b, sizeof(b), "FX %d", info->fxCount);
        DrawButton(this, L.fx, b, info->fxCount > 0, accent);
        std::snprintf(b, sizeof(b), "Snd %d", info->sendCount);
        DrawButton(this, L.sends, b, info->sendCount > 0, accent);
        std::snprintf(b, sizeof(b), "\xE2\x86\x92%s", info->outLabel.c_str());
        DrawButton(this, L.out, b, info->outLabel != "Mst", Rgb(70, 90, 130));
        DrawButton(this, L.autob, "Read", false, ColMidiAccent());
        if (type == 1) DrawButton(this, L.inst, "Inst", true, Rgb(70, 90, 130));

        // Pan knob + value readout.
        DrawKnob(this, L.pan, info->pan, accent);
        char pv[24];
        const float d = gain > 1e-4f ? 20.0f * std::log10(gain) : -80.0f;
        std::snprintf(pv, sizeof(pv), "%+.0f  %.1fdB",
                      info->pan * 50.0f, d <= -80.0f ? -99.9f : d);
        SetHighColor(ColLcd());  FillRect(L.val);
        SetHighColor(ColLcdText());
        DrawString(pv, BPoint(L.val.left + 3, L.val.bottom - 3));
    }

    // Fader + VU meter.
    DrawFader(this, L.fader, GainToFrac(gain), 1.0f / kMaxGain);
    DrawVUMeter(this, L.meter, std::max(peakL, peakR));
    // dB scale between fader and meter.
    SetHighColor(ColTextDim());
    for (float gm : { 1.5f, 1.0f, 0.5f, 0.25f, 0.12f }) {
        const float yy = L.fader.bottom - L.fader.Height() * (gm / kMaxGain);
        if (yy < L.fader.top || yy > L.fader.bottom) continue;
        StrokeLine(BPoint(L.fader.right + 2, yy), BPoint(L.fader.right + 5, yy));
    }

    // Input-monitor / Record / Mute / Solo (track strips only).
    if (info) {
        DrawButton(this, L.mon,  "I", info->inputMonitor, ColMon());
        DrawButton(this, L.arm,  "R", info->armed,        ColRec());
        DrawButton(this, L.mute, "M", info->muted,        ColMute());
        DrawButton(this, L.solo, "S", info->soloed,       ColSolo());
    } else {
        char db[16];
        if (gain <= 1e-4f) std::snprintf(db, sizeof(db), "-inf");
        else               std::snprintf(db, sizeof(db), "%+.1f dB", 20.0f * std::log10(gain));
        SetHighColor(ColText());
        DrawString(db, BPoint(L.mon.left, L.mute.bottom - 2));
    }

    // Colored name label along the bottom (Logic channel-strip signature).
    SetHighColor(tc);
    FillRect(L.name);
    const int lum = (tc.red * 30 + tc.green * 59 + tc.blue * 11) / 100;
    SetHighColor(lum > 140 ? Rgb(20, 20, 22) : Rgb(245, 246, 248));
    DrawString(info ? info->name.c_str() : "Master",
               BPoint(L.name.left + 5, L.name.bottom - 6));

    SetHighColor(ColGrid());
    StrokeRect(strip);
}

void MixerStripsView::Draw(BRect) {
    for (size_t i = 0; i < fStrips.size(); i++) {
        const MixerStripInfo& s = fStrips[i];
        float pl = 0, pr = 0;
        if (auto it = fPeaks.find(s.trackId); it != fPeaks.end()) {
            pl = it->second.first; pr = it->second.second;
        }
        DrawStrip((int)i, &s, s.gain, pl, pr, false);
    }
    DrawStrip((int)fStrips.size(), nullptr, fMasterGain, fMasterL, fMasterR, true);
}

int MixerStripsView::StripAt(BPoint where) const {
    const int total = (int)fStrips.size() + 1;   // + master
    for (int i = 0; i < total; i++) {
        const float x0 = StripX(i);
        if (where.x >= x0 && where.x <= x0 + kStripW) return i;
    }
    return -1;
}

void MixerStripsView::ApplyStrip(int i) {
    if (i == (int)fStrips.size()) {   // master
        BMessage m(kMsgApplyMaster);
        m.AddFloat("gain", fMasterGain);
        fApply.SendMessage(&m);
        return;
    }
    if (i < 0 || i >= (int)fStrips.size()) return;
    const MixerStripInfo& s = fStrips[i];
    BMessage m(kMsgApplyMix);
    m.AddInt64("track", (int64)s.trackId);
    m.AddFloat("gain", s.gain);
    m.AddFloat("pan", s.pan);
    m.AddBool("mute", s.muted);
    m.AddBool("solo", s.soloed);
    fApply.SendMessage(&m);
}

void MixerStripsView::Post(uint32 what, uint64 track) {
    BMessage m(what);
    m.AddInt64("track", (int64)track);
    fApply.SendMessage(&m);
}

void MixerStripsView::MouseDown(BPoint where) {
    const int i = StripAt(where);
    if (i < 0) return;
    const float x0 = StripX(i);
    const float h  = Bounds().Height();
    const bool master = (i == (int)fStrips.size());
    const StripLayout L = LayoutStrip(x0, h);

    if (L.fader.Contains(where)) {
        fDrag = Drag::Fader; fDragStrip = i;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        MouseMoved(where, 0, nullptr);
        return;
    }
    if (master) return;
    MixerStripInfo& s = fStrips[i];
    if (L.pan.Contains(where)) {
        fDrag = Drag::Pan; fDragStrip = i; fPanGrabY = where.y; fPanOrig = s.pan;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }
    if (L.mute.Contains(where)) { s.muted = !s.muted; ApplyStrip(i); Invalidate(); return; }
    if (L.solo.Contains(where)) { s.soloed = !s.soloed; ApplyStrip(i); Invalidate(); return; }
    if (L.arm.Contains(where))  { s.armed = !s.armed; Post(kMsgMixArm, s.trackId); Invalidate(); return; }
    if (L.mon.Contains(where))  { s.inputMonitor = !s.inputMonitor; Post(kMsgMixMon, s.trackId); Invalidate(); return; }
    if (L.fx.Contains(where))    { Post(kMsgMixFx, s.trackId);    return; }
    if (L.sends.Contains(where)) { Post(kMsgMixSends, s.trackId); return; }
    if (s.type == 1 && L.inst.Contains(where)) { Post(kMsgMixInst, s.trackId); return; }
    // Any other click on the strip selects the track (edit in the inspector).
    Post(kMsgMixSelect, s.trackId);
}

void MixerStripsView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None || fDragStrip < 0) return;
    const float x0 = StripX(fDragStrip);
    const float h  = Bounds().Height();
    const bool master = (fDragStrip == (int)fStrips.size());
    const StripLayout L = LayoutStrip(x0, h);

    if (fDrag == Drag::Fader) {
        const float fh = L.fader.Height();
        float t = fh > 0 ? (L.fader.bottom - where.y) / fh : 0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        const float gain = t * kMaxGain;
        if (master) fMasterGain = gain;
        else        fStrips[fDragStrip].gain = gain;
    } else {   // Pan knob: vertical drag (up = right) relative to grab.
        float p = fPanOrig + (fPanGrabY - where.y) / 100.0f;
        if (p < -1) p = -1; if (p > 1) p = 1;
        fStrips[fDragStrip].pan = p;
    }
    ApplyStrip(fDragStrip);
    Invalidate();
}

void MixerStripsView::MouseUp(BPoint) {
    fDrag = Drag::None;
    fDragStrip = -1;
}

// --- window ---------------------------------------------------------------

MixerWindow::MixerWindow(BRect frame, std::vector<MixerStripInfo> strips,
                         float masterGain, BMessenger apply)
    : BWindow(frame, "Mixer", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fApply(apply) {
    SetSizeLimits(160, 100000, 320, 100000);   // keep the strip tall enough
    fView = new MixerStripsView(Bounds(), std::move(strips), masterGain, apply);
    AddChild(fView);
}

void MixerWindow::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardSpaceToTransport(m, fApply)) return;
    BWindow::DispatchMessage(m, h);
}

void MixerWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgMixPeaks) {
        std::map<uint64, std::pair<float, float>> peaks;
        int64 tid = 0; float pl = 0, pr = 0;
        for (int32 i = 0; msg->FindInt64("tid", i, &tid) == B_OK; i++) {
            msg->FindFloat("pl", i, &pl);
            msg->FindFloat("pr", i, &pr);
            peaks[(uint64)tid] = { pl, pr };
        }
        float ml = 0, mr = 0;
        msg->FindFloat("mpl", &ml);
        msg->FindFloat("mpr", &mr);
        fView->SetPeaks(peaks, ml, mr);
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
