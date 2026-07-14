#include "MixerWindow.h"

#include "UiMetrics.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace daw {

// Strip geometry.
static constexpr float kStripW  = 84.0f;
static constexpr float kGap     = 6.0f;
static constexpr float kPadT    = 26.0f;   // name band height
static constexpr float kMaxGain = 1.5f;

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

// Fader / meter / pan / button sub-rects within a strip at left x0.
static BRect FaderRect(float x0, float h) {
    return BRect(x0 + 8, kPadT + 10, x0 + 24, h - 96);
}
static BRect MeterRect(float x0, float h) {
    return BRect(x0 + 30, kPadT + 10, x0 + 48, h - 96);
}
static BRect PanBarRect(float x0, float h) {
    return BRect(x0 + 8, h - 82, x0 + kStripW - 8, h - 70);
}
static BRect MuteRect(float x0, float h) {
    return BRect(x0 + 8, h - 60, x0 + 40, h - 40);
}
static BRect SoloRect(float x0, float h) {
    return BRect(x0 + 44, h - 60, x0 + kStripW - 8, h - 40);
}

void MixerStripsView::DrawStrip(int i, const char* name, float gain, float pan,
                                bool muted, bool soloed, int colorIndex,
                                float peakL, float peakR, bool master) {
    const float x0 = StripX(i);
    const float h  = Bounds().Height();
    BRect strip(x0, 4, x0 + kStripW, h - 6);

    SetHighColor(ColHeader());
    FillRect(strip);
    SetHighColor(master ? Rgb(150, 120, 70) : TrackColor(colorIndex));
    FillRect(BRect(strip.left, strip.top, strip.right, strip.top + kPadT - 6));
    SetHighColor(ColText());
    DrawString(name, BPoint(strip.left + 6, strip.top + 15));

    // Fader groove + fill.
    BRect fr = FaderRect(x0, h);
    SetHighColor(Rgb(16, 18, 22));
    FillRect(fr);
    float g = gain / kMaxGain; if (g < 0) g = 0; if (g > 1) g = 1;
    BRect fill = fr; fill.top = fr.bottom - fr.Height() * g;
    SetHighColor(master ? Rgb(200, 165, 90) : ColAccent());
    FillRect(fill);
    const float uy = fr.bottom - fr.Height() * (1.0f / kMaxGain);   // unity tick
    SetHighColor(ColTextDim());
    StrokeLine(BPoint(fr.left, uy), BPoint(fr.right, uy));
    SetHighColor(ColGrid());
    StrokeRect(fr);
    // Fader thumb (cap at the current level). Kept inside the fader rect so it
    // stays fully clickable (MouseDown hit-tests FaderRect).
    float ty = fill.top;
    if (ty < fr.top + 2)    ty = fr.top + 2;
    if (ty > fr.bottom - 2) ty = fr.bottom - 2;
    SetHighColor(ColText());
    FillRect(BRect(fr.left, ty - 2, fr.right, ty + 2));
    SetHighColor(Rgb(20, 22, 26));
    StrokeLine(BPoint(fr.left, ty), BPoint(fr.right, ty));

    // dB readout.
    char db[16];
    if (gain <= 0.0001f) std::snprintf(db, sizeof(db), "-inf");
    else                 std::snprintf(db, sizeof(db), "%+.1f", 20.0f * std::log10(gain));
    SetHighColor(ColTextDim());
    DrawString(db, BPoint(x0 + 6, h - 88));

    // Stereo meter.
    BRect mr = MeterRect(x0, h);
    SetHighColor(Rgb(16, 18, 22));
    FillRect(mr);
    const float bw = (mr.Width() - 3) * 0.5f;
    auto bar = [&](float bx, float lvl) {
        if (lvl < 0) lvl = 0; if (lvl > 1) lvl = 1;
        BRect b(bx, mr.bottom - mr.Height() * lvl, bx + bw, mr.bottom);
        SetHighColor(MeterColor(lvl));
        FillRect(b);
    };
    bar(mr.left + 1, peakL);
    bar(mr.left + 2 + bw, peakR);
    // dB tick lines (0 / -6 / -12 dBFS).
    for (float lvl : { 1.0f, 0.5f, 0.25f }) {
        const float ty = mr.bottom - mr.Height() * lvl;
        SetHighColor(ColGrid());
        StrokeLine(BPoint(mr.left, ty), BPoint(mr.right, ty));
    }
    SetHighColor(ColGrid());
    StrokeRect(mr);

    // Pan bar (master has none).
    if (!master) {
        BRect pr = PanBarRect(x0, h);
        SetHighColor(Rgb(24, 26, 31));
        FillRect(pr);
        const float cx = (pr.left + pr.right) * 0.5f;
        SetHighColor(ColGrid());
        StrokeLine(BPoint(cx, pr.top), BPoint(cx, pr.bottom));
        const float px = cx + (pan * 0.5f) * pr.Width();
        SetHighColor(ColAccent());
        FillRect(BRect(px - 2, pr.top, px + 2, pr.bottom));
        SetHighColor(ColGrid());
        StrokeRect(pr);

        BRect m = MuteRect(x0, h), s = SoloRect(x0, h);
        SetHighColor(muted ? ColPlayhead() : ColHeaderHi());
        FillRect(m);
        SetHighColor(soloed ? Rgb(210, 190, 70) : ColHeaderHi());
        FillRect(s);
        SetHighColor(ColGrid()); StrokeRect(m); StrokeRect(s);
        SetHighColor(ColText());
        DrawString("M", BPoint(m.left + 12, m.bottom - 6));
        DrawString("S", BPoint(s.left + 12, s.bottom - 6));
    }

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
        DrawStrip((int)i, s.name.c_str(), s.gain, s.pan, s.muted, s.soloed,
                  s.colorIndex, pl, pr, false);
    }
    DrawStrip((int)fStrips.size(), "Master", fMasterGain, 0.0f, false, false,
              0, fMasterL, fMasterR, true);
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

void MixerStripsView::MouseDown(BPoint where) {
    const int i = StripAt(where);
    if (i < 0) return;
    const float x0 = StripX(i);
    const float h  = Bounds().Height();
    const bool master = (i == (int)fStrips.size());

    if (FaderRect(x0, h).Contains(where)) {
        fDrag = Drag::Fader; fDragStrip = i;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        MouseMoved(where, 0, nullptr);
        return;
    }
    if (!master && PanBarRect(x0, h).Contains(where)) {
        fDrag = Drag::Pan; fDragStrip = i;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        MouseMoved(where, 0, nullptr);
        return;
    }
    if (!master && MuteRect(x0, h).Contains(where)) {
        fStrips[i].muted = !fStrips[i].muted; ApplyStrip(i); Invalidate(); return;
    }
    if (!master && SoloRect(x0, h).Contains(where)) {
        fStrips[i].soloed = !fStrips[i].soloed; ApplyStrip(i); Invalidate(); return;
    }
}

void MixerStripsView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None || fDragStrip < 0) return;
    const float x0 = StripX(fDragStrip);
    const float h  = Bounds().Height();
    const bool master = (fDragStrip == (int)fStrips.size());

    if (fDrag == Drag::Fader) {
        BRect fr = FaderRect(x0, h);
        float t = (fr.bottom - where.y) / fr.Height();
        if (t < 0) t = 0; if (t > 1) t = 1;
        const float gain = t * kMaxGain;
        if (master) fMasterGain = gain;
        else        fStrips[fDragStrip].gain = gain;
    } else {   // Pan
        BRect pr = PanBarRect(x0, h);
        float p = ((where.x - pr.left) / pr.Width()) * 2.0f - 1.0f;
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
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
    fView = new MixerStripsView(Bounds(), std::move(strips), masterGain, apply);
    AddChild(fView);
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
