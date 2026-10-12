#include "MixerWindow.h"

#include "UiMetrics.h"
#include "Widgets.h"
#include "widgets/DawTextField.h"   // the inline dB entry (M3.4)
#include "../model/types.h"         // kTrackTrimMax (the trim knob's ceiling)

#include <String.h>
#include <TextView.h>   // the inline level field's SelectAll

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

// Insert slot list. Rows are tighter than the inspector's (17 px) because a
// mixer strip is 96 px wide and shares its height with five other sections; the
// cap is lower for the same reason -- four rows is already 60 px of a strip that
// still has to fit a pan knob, a fader and four buttons.
static constexpr float kFxRowH  = 15.0f;
static constexpr int   kMaxFxRows = 4;
static constexpr float kFxDotW  = 11.0f;
// Aux sends drawn on the strip (M3.4): two fit before the strip stops reading
// like a console strip. A longer list goes through the Sends button.
static constexpr int   kMaxSendRows = 2;
static constexpr float kSendRowH = 18.0f;
// The trim knob and its two neighbours (the phase button and the value text).
static constexpr float kTrimRowH = 24.0f;

// All the sub-control rects of one strip, laid out top -> bottom.
struct StripLayout {
    BRect input, fx, sends, out, autob, inst;   // section buttons
    BRect send[2];                              // per-send rows (M3.4)
    BRect sendKnob[2], sendLabel[2], sendVal[2];
    BRect trim, trimKnob, trimVal, phase;       // input trim + polarity (M3.4)
    BRect pan, val, fader, meter;
    BRect mon, arm, mute, solo, sgroup, name;
};

// `fxRows`/`sendRows` are the blocks every strip reserves (see FxRows() and
// SendRows()), so the sections below them line up across the whole rack.
static StripLayout LayoutStrip(float x0, float h, int fxRows, int sendRows) {
    StripLayout s;
    const float L = x0 + 6, R = x0 + kStripW - 6;
    const float cx = x0 + kStripW * 0.5f;
    float y = kPadT + 4;
    auto row = [&](float ht) { BRect r(L, y, R, y + ht); y += ht + 3; return r; };
    s.input = row(15);
    // The insert block. Sized so it ends exactly on the last row's bottom edge,
    // because MouseDown gates on the block before searching the rows: a block
    // one pixel short would make the last row's bottom pixel unclickable.
    s.fx    = row(fxRows * kFxRowH - 2.0f);
    s.sends = row(15);
    for (int i = 0; i < sendRows; i++) {
        s.send[i] = row(kSendRowH - 2.0f);
        // A row is a label, the level knob and the level as text: the knob on
        // the right half, where a hand expects a level control to be.
        s.sendLabel[i] = BRect(s.send[i].left, s.send[i].top,
                               s.send[i].left + 30, s.send[i].bottom);
        s.sendKnob[i]  = BRect(s.send[i].right - 50, s.send[i].top - 1,
                               s.send[i].right - 30, s.send[i].bottom + 1);
        s.sendVal[i]   = BRect(s.send[i].right - 26, s.send[i].top,
                               s.send[i].right, s.send[i].bottom);
    }
    s.out   = row(15);
    s.autob = row(15);
    s.inst  = row(15);            // MIDI only
    y += 4;
    // Input trim (a knob, unity at 12 o'clock) with its value and the phase
    // button beside it. They sit above the pan knob: trim, polarity, pan is
    // the order the signal meets them.
    s.trim    = row(kTrimRowH);
    s.trimKnob = BRect(s.trim.left, s.trim.top, s.trim.left + 24, s.trim.bottom);
    s.trimVal  = BRect(s.trim.left + 27, s.trim.top, s.trim.left + 55,
                       s.trim.bottom);
    s.phase    = BRect(s.trim.right - 22, s.trim.top + 1, s.trim.right,
                       s.trim.bottom - 1);
    y += 2;
    s.pan = BRect(cx - 21, y, cx + 21, y + 42); y += 46;
    s.val = BRect(L, y, R, y + 14); y += 16;
    // Reserve room for I/R, M/S/G, name; never let the fader invert / zero-out
    // (guards a div-by-zero in the fader drag on a very short window).
    const float botTop = std::max(h - 76.0f, y + 20.0f);
    s.fader = BRect(cx - 30, y, cx - 4, botTop);
    s.meter = BRect(cx + 6,  y, cx + 26, botTop);
    float by = h - 72;
    s.mon  = BRect(L, by, cx - 2, by + 18);
    s.arm  = BRect(cx + 2, by, R, by + 18); by += 22;
    // Mute, Solo and the solo-group box share the row (M3.4): a solo group is
    // a per-strip choice, and the box says which one the strip is in.
    s.mute   = BRect(L, by, L + 34, by + 18);
    s.solo   = BRect(L + 36, by, L + 58, by + 18);
    s.sgroup = BRect(L + 60, by, R, by + 18);
    s.name = BRect(x0, h - 24, x0 + kStripW, h - 6);
    return s;
}

// One row inside a strip's insert block. Drawing and hit-testing both come
// through here so they cannot disagree about where a row is.
static BRect FxRowRect(const StripLayout& L, int i) {
    const float top = L.fx.top + i * kFxRowH;
    return BRect(L.fx.left, top, L.fx.right, top + kFxRowH - 2.0f);
}

// The trim knob's mapping (M3.4): unity sits at 12 o'clock, the ceiling
// (kTrackTrimMax = +12 dB) at the top of the sweep and silence at the bottom.
// A plain `trim / max` would put unity at a quarter turn, which reads wrong on
// a control whose whole meaning is "is it at unity?".
float MixerStripsView::TrimToKnob(float trim) {
    if (!(trim >= 0.0f)) return -1.0f;
    if (trim >= 1.0f) {
        const float top = kTrackTrimMax > 1.0f ? kTrackTrimMax - 1.0f : 1.0f;
        const float v = (trim - 1.0f) / top;
        return v > 1.0f ? 1.0f : v;
    }
    return trim - 1.0f;              // silence (-1) .. unity (0)
}

float MixerStripsView::KnobToTrim(float knob) {
    if (knob < -1.0f) knob = -1.0f;
    if (knob >  1.0f) knob =  1.0f;
    if (knob >= 0.0f) return 1.0f + knob * (kTrackTrimMax - 1.0f);
    return 1.0f + knob;              // 0 .. 1
}

int MixerStripsView::FxRowsFor(const MixerStripInfo& s) {
    const int n = (int)s.fx.size();
    // Under the cap: one row per insert plus a trailing empty slot to add into.
    // At or over it: the last row becomes "+N more", which opens the editor --
    // so a long chain has no empty row, and adding goes through the editor.
    return n < kMaxFxRows ? n + 1 : kMaxFxRows;
}

void MixerStripsView::RecomputeFxRows() {
    int rows = 1;                    // the master strip still reserves the block
    for (const MixerStripInfo& s : fStrips)
        rows = std::max(rows, FxRowsFor(s));
    fFxRows = rows;
    // The send rows are the same idea (M3.4): every strip reserves what the
    // deepest strip needs, so the pan knobs, faders and meters stay on one
    // line across the rack. 0 when nothing has a send, which is most projects.
    int srows = 0;
    for (const MixerStripInfo& s : fStrips)
        srows = std::max(srows, SendRowsFor(s));
    fSendRows = srows;
}

int MixerStripsView::SendRowsFor(const MixerStripInfo& s) {
    return std::min<int>((int)s.sends.size(), kMaxSendRows);
}

MixerStripsView::MixerStripsView(BRect frame, std::vector<MixerStripInfo> strips,
                                 float masterGain, BMessenger apply)
    : BView(frame, "strips", B_FOLLOW_ALL_SIDES, B_WILL_DRAW),
      fStrips(std::move(strips)), fMasterGain(masterGain), fApply(apply) {
    ApplyTheme();
    RecomputeFxRows();
}

void MixerStripsView::ApplyTheme() {
    SetViewColor(ColBackground());
}

float MixerStripsView::StripX(int i) const {
    return 8.0f + i * (kStripW + kGap) - fScrollX;
}

// Every strip, the master, and the margins -- what the rack needs to be shown
// whole. Used to clamp the horizontal pan.
float MixerStripsView::ContentWidth() const {
    return 8.0f + ((float)fStrips.size() + 1.0f) * (kStripW + kGap) + 8.0f;
}

// The wheel pans the rack sideways when it is wider than the view (M1.4): a
// console with more strips than fit is the normal case on a small screen, and
// a mixer that silently cuts the last strips off is worse than one that moves.
void MixerStripsView::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgMixLevelCommit) {   // the inline dB field's Enter
        CommitLevelEntry();
        return;
    }
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f, dx = 0.0f;
        msg->FindFloat("be:wheel_delta_y", &dy);
        msg->FindFloat("be:wheel_delta_x", &dx);
        const float move = (dx != 0.0f ? dx : dy) * 40.0f;
        const float maxScroll = ContentWidth() - Bounds().Width();
        if (maxScroll <= 0.0f) { fScrollX = 0.0f; return; }
        fScrollX += move;
        if (fScrollX < 0.0f) fScrollX = 0.0f;
        if (fScrollX > maxScroll) fScrollX = maxScroll;
        Invalidate();
        return;
    }
    BView::MessageReceived(msg);
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
    const StripLayout L = LayoutStrip(x0, h, FxRows(), SendRows());
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

        // --- Insert slots ---------------------------------------------------
        // Same shape as the inspector's list, narrower: a row per insert with
        // its name and a bypass dot, a trailing empty row to add into, and an
        // overflow row when the chain outgrows the block.
        const int nfx  = (int)info->fx.size();
        const int rows = FxRowsFor(*info);
        for (int r = 0; r < rows; r++) {
            const BRect rr = FxRowRect(L, r);
            const bool overflow = nfx >= kMaxFxRows && r == kMaxFxRows - 1;
            const bool empty    = !overflow && r >= nfx;

            SetHighColor(ColHeaderHi());
            FillRect(rr);
            SetHighColor(ColGrid());
            StrokeRect(rr);

            if (overflow) {
                std::snprintf(b, sizeof(b), "+%d more", nfx - (kMaxFxRows - 1));
                SetHighColor(ColTextDim());
                DrawString(b, BPoint(rr.left + 4, rr.bottom - 4));
                continue;
            }
            if (empty) {
                SetHighColor(ColTextDim());
                DrawString("+ Add", BPoint(rr.left + 4, rr.bottom - 4));
                continue;
            }

            const MixerInsertInfo& ins = info->fx[(size_t)r];
            SetHighColor(ins.bypassed ? ColTextDim() : ColText());
            BString nm(ins.name.c_str());
            TruncateString(&nm, B_TRUNCATE_END, rr.Width() - kFxDotW - 8.0f);
            DrawString(nm.String(), BPoint(rr.left + 4, rr.bottom - 4));

            BRect dot(rr.right - kFxDotW - 2, rr.top + 3,
                      rr.right - 4, rr.top + 3 + (kFxDotW - 5));
            if (ins.bypassed) {
                SetHighColor(ColTextDim());
                StrokeEllipse(dot);
            } else {
                SetHighColor(accent);
                FillEllipse(dot);
            }
        }
        // The row the dragged insert will OCCUPY, framed -- the inspector's
        // convention, and for the same reason: the drop target is the row under
        // the pointer, so it names a slot rather than a gap between slots.
        if (fDrag == Drag::FxSlot && fDragStrip == i
            && fDragFxTo >= 0 && fDragFxTo < rows) {
            SetHighColor(accent);
            StrokeRect(FxRowRect(L, fDragFxTo));
        }

        // The sends button names how many the track has; the rows under it are
        // the levels themselves (M3.4). "+N" says the rest are in the editor.
        const int nsend = (int)info->sends.size();
        if (nsend > kMaxSendRows)
            std::snprintf(b, sizeof(b), "Snd +%d", nsend - kMaxSendRows);
        else
            std::snprintf(b, sizeof(b), "Snd %d", nsend);
        DrawButton(this, L.sends, b, nsend > 0, accent);
        for (int k = 0; k < SendRows(); k++) {
            if (k >= nsend) continue;         // the block is reserved, not drawn
            const MixerSendInfo& send = info->sends[(size_t)k];
            SetHighColor(ColTextDim());
            BString nm(send.label.c_str());
            TruncateString(&nm, B_TRUNCATE_END,
                           L.sendLabel[k].Width() - 2.0f);
            DrawString(nm.String(), BPoint(L.sendLabel[k].left + 2,
                                           L.sendLabel[k].bottom - 4));
            // The level knob: 0 .. 1.5 mapped to the knob's own sweep with
            // unity at the top end (a send is rarely meant to exceed unity).
            float v = send.level / 1.5f;
            if (v > 1.0f) v = 1.0f;
            if (!(v >= 0.0f)) v = 0.0f;
            DrawKnob(this, L.sendKnob[k], v * 2.0f - 1.0f, accent, false);
            std::snprintf(b, sizeof(b), "%.2f", send.level);
            SetHighColor(ColTextDim());
            DrawString(b, BPoint(L.sendVal[k].left, L.sendVal[k].bottom - 4));
        }
        std::snprintf(b, sizeof(b), "\xE2\x86\x92%s", info->outLabel.c_str());
        DrawButton(this, L.out, b, info->outLabel != "Mst", Rgb(70, 90, 130));
        DrawButton(this, L.autob, "Read", false, ColMidiAccent());
        if (type == 1) DrawButton(this, L.inst, "Inst", true, Rgb(70, 90, 130));

        // Input trim (unity at 12 o'clock), its value in dB, and the phase
        // button beside it (M3.4).
        DrawKnob(this, L.trimKnob, TrimToKnob(info->trim), accent);
        {
            const float d = info->trim > 1e-4f
                          ? 20.0f * std::log10(info->trim) : -80.0f;
            if (std::fabs(d) < 0.05f)
                std::snprintf(b, sizeof(b), "--");
            else if (d <= -80.0f)
                std::snprintf(b, sizeof(b), "-inf");
            else
                std::snprintf(b, sizeof(b), "%+.1f", d);
            SetHighColor(ColTextDim());
            BString tv(b);
            TruncateString(&tv, B_TRUNCATE_END, L.trimVal.Width() - 2.0f);
            DrawString(tv.String(), BPoint(L.trimVal.left,
                                           L.trimVal.bottom - 6));
        }
        DrawButton(this, L.phase, "\xC3\x98", info->phaseInvert,
                   ColRec());   // the polarity lamp reads as the record red

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

    // Fader + meter. The meter is the shared dB-scaled bar (M3.3); the ticks
    // beside it are the same dB scale (MeterMath's list), so the strip reads
    // as a dB meter rather than as a filled rectangle.
    DrawFader(this, L.fader, GainToFrac(gain), 1.0f / kMaxGain);
    DrawMeterBar(this, L.meter, std::max(peakL, peakR), false);
    SetHighColor(ColTextDim());
    for (int i = 0; i < kMeterTickCount; i++) {
        const float yy = L.meter.bottom - L.meter.Height() * DbToFrac(kMeterTicksDb[i]);
        if (yy < L.meter.top || yy > L.meter.bottom) continue;
        StrokeLine(BPoint(L.meter.right + 2, yy), BPoint(L.meter.right + 5, yy));
    }

    // Input-monitor / Record / Mute / Solo / Solo-group (track strips only).
    if (info) {
        DrawButton(this, L.mon,  "I", info->inputMonitor, ColMon());
        DrawButton(this, L.arm,  "R", info->armed,        ColRec());
        DrawButton(this, L.mute, "M", info->muted,        ColMute());
        DrawButton(this, L.solo, "S", info->soloed,       ColSolo());
        // The solo group (M3.4): "G" alone means none, "G2" the group's id.
        char g[8];
        if (info->soloGroup > 0) std::snprintf(g, sizeof(g), "G%d", info->soloGroup);
        else                     std::snprintf(g, sizeof(g), "G");
        DrawButton(this, L.sgroup, g, info->soloGroup > 0, ColSolo());
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
    SetHighColor(LabelOn(tc));   // the theme's rule: whichever of the two reads
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
    // The strip extras (M3.4). The window's handler diffs every field against
    // the model and only the changed ones become commands, so sending the
    // whole strip state on every gesture stays one clean undo step.
    m.AddFloat("trim", s.trim);
    m.AddBool("phase", s.phaseInvert);
    m.AddInt32("sgroup", s.soloGroup);
    fApply.SendMessage(&m);
}

// One send's level (M3.4). The destination is addressed by INDEX into the
// track's send list — the strip drew that list, and a knob drag is one
// gesture, so an index that has moved under it is the same hazard the insert
// reorder already handles by naming the track in the message.
void MixerStripsView::PostSendLevel(int i, int index, float level) {
    if (i < 0 || i >= (int)fStrips.size()) return;
    BMessage m(kMsgMixSend);
    m.AddInt64("track", (int64)fStrips[i].trackId);
    m.AddInt32("index", index);
    m.AddFloat("level", level);
    fApply.SendMessage(&m);
}

// --- the inline dB entry (M3.4) -------------------------------------------

void MixerStripsView::BeginLevelEntry(int strip) {
    if (strip < 0 || strip >= (int)fStrips.size()) return;
    const float x0 = StripX(strip);
    const StripLayout L = LayoutStrip(x0, Bounds().Height(), FxRows(),
                                      SendRows());
    // The field covers the strip's value row, so what is being typed is where
    // the value it replaces was.
    BRect r = L.val;
    r.top -= 3.0f; r.bottom += 3.0f;
    if (fLevelField == nullptr) {
        fLevelField = new DawTextField(r, "mixlevel", NULL, "",
                                       new BMessage(kMsgMixLevelCommit),
                                       B_FOLLOW_LEFT_TOP);
        fLevelField->SetDivider(0.0f);
        fLevelField->SetTarget(this);
        AddChild(fLevelField);
    } else {
        fLevelField->MoveTo(r.left, r.top);
        fLevelField->ResizeTo(r.Width(), r.Height());
    }
    const MixerStripInfo& s = fStrips[strip];
    char db[32];
    const float d = s.gain > 1e-4f ? 20.0f * std::log10(s.gain) : -80.0f;
    if (d <= -80.0f) std::snprintf(db, sizeof(db), "-inf");
    else             std::snprintf(db, sizeof(db), "%.1f", d);
    fLevelStrip = strip;
    fLevelField->SetText(db);
    fLevelField->Show();
    fLevelField->MakeFocus(true);
    // Select what is there, so typing replaces the old figure instead of
    // appending to it.
    if (BTextView* tv = fLevelField->TextView()) {
        tv->SelectAll();
    }
}

void MixerStripsView::CommitLevelEntry() {
    if (fLevelStrip < 0 || fLevelField == nullptr) return;
    const int strip = fLevelStrip;
    const std::string typed = fLevelField->Text() ? fLevelField->Text() : "";
    float db = 0.0f;
    if (ParseDecibels(typed, &db) && strip < (int)fStrips.size()) {
        float gain = GainFromDb(db);
        if (gain > kMaxGain) gain = kMaxGain;    // the fader's own range
        fStrips[strip].gain = gain;
        ApplyStrip(strip);
    }
    // A value that could not be read is simply not applied: the fader stays
    // where it was, which is the honest outcome for a mistyped level.
    CancelLevelEntry();
}

void MixerStripsView::CancelLevelEntry() {
    fLevelStrip = -1;
    if (fLevelField != nullptr) fLevelField->Hide();
    Invalidate();
}

void MixerStripsView::SetStrips(std::vector<MixerStripInfo> strips, float masterGain) {
    if (fDrag != Drag::None) return;   // don't disrupt an in-progress gesture
    // ... nor an open typed-level field: the strip it is over may have moved.
    if (fLevelStrip >= 0) CancelLevelEntry();
    fStrips = std::move(strips);
    fMasterGain = masterGain;
    RecomputeFxRows();
    Invalidate();
}

void MixerStripsView::Post(uint32 what, uint64 track) {
    BMessage m(what);
    m.AddInt64("track", (int64)track);
    fApply.SendMessage(&m);
}

void MixerStripsView::MouseDown(BPoint where) {
    // Any click outside the inline level field ends that gesture first: the
    // field is a child view, so a click on the strip behind it never reaches
    // it, and leaving it up would hide the value it is covering.
    if (fLevelStrip >= 0 && fLevelField != nullptr) CancelLevelEntry();

    const int i = StripAt(where);
    if (i < 0) return;
    const float x0 = StripX(i);
    const float h  = Bounds().Height();
    const bool master = (i == (int)fStrips.size());
    const StripLayout L = LayoutStrip(x0, h, FxRows(), SendRows());

    BMessage* msg = Window() ? Window()->CurrentMessage() : nullptr;
    int32 clicks = 1;
    if (msg) msg->FindInt32("clicks", &clicks);

    // A double-click on the fader or on the value readout types a dB value
    // (M3.4) instead of dragging.
    if (clicks >= 2 && (L.fader.Contains(where) || L.val.Contains(where))) {
        BeginLevelEntry(i);
        return;
    }

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
    // The input trim and the per-send levels: the same vertical-drag gesture
    // as the pan knob.
    if (L.trimKnob.Contains(where)) {
        fDrag = Drag::Trim; fDragStrip = i; fTrimGrabY = where.y;
        fTrimOrig = s.trim;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }
    for (int k = 0; k < SendRows() && k < (int)s.sends.size(); k++) {
        if (!L.sendKnob[k].Contains(where)) continue;
        fDrag = Drag::Send; fDragStrip = i; fSendIndex = k;
        fSendGrabY = where.y; fSendOrig = s.sends[(size_t)k].level;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }
    if (L.phase.Contains(where)) {
        s.phaseInvert = !s.phaseInvert; ApplyStrip(i); Invalidate(); return;
    }
    // The solo group cycles none -> 1 -> 2 -> 3 -> none. A click is the whole
    // gesture, so it is one undo step each time.
    if (L.sgroup.Contains(where)) {
        s.soloGroup = (s.soloGroup >= 3) ? 0 : s.soloGroup + 1;
        ApplyStrip(i);
        Invalidate();
        return;
    }
    if (L.mute.Contains(where)) { s.muted = !s.muted; ApplyStrip(i); Invalidate(); return; }
    if (L.solo.Contains(where)) { s.soloed = !s.soloed; ApplyStrip(i); Invalidate(); return; }
    if (L.arm.Contains(where))  { s.armed = !s.armed; Post(kMsgMixArm, s.trackId); Invalidate(); return; }
    if (L.mon.Contains(where))  { s.inputMonitor = !s.inputMonitor; Post(kMsgMixMon, s.trackId); Invalidate(); return; }
    if (L.fx.Contains(where)) {
        const int nfx  = (int)s.fx.size();
        const int rows = FxRowsFor(s);
        for (int r = 0; r < rows; r++) {
            if (!FxRowRect(L, r).Contains(where)) continue;
            const bool overflow = nfx >= kMaxFxRows && r == kMaxFxRows - 1;
            const bool empty    = !overflow && r >= nfx;

            if (overflow) { Post(kMsgMixFx, s.trackId); return; }
            if (empty)    { Post(kMsgMixFxAdd, s.trackId); return; }

            if (where.x >= FxRowRect(L, r).right - kFxDotW - 4) {
                // Toggle the snapshot so the dot responds now, and post the
                // intent. The main window runs the command and pushes the whole
                // strip state back, which is what the drawing ends up on.
                s.fx[(size_t)r].bypassed = !s.fx[(size_t)r].bypassed;
                BMessage m(kMsgMixFxBypass);
                m.AddInt64("track", (int64)s.trackId);
                m.AddInt32("fx", r);
                fApply.SendMessage(&m);
                Invalidate();
                return;
            }

            // Anywhere else: a possible reorder drag. A click that never moves
            // opens the editor on that insert (see MouseUp).
            fDrag = Drag::FxSlot;
            fDragStrip = i;
            fDragFxFrom = fDragFxTo = r;
            fDragFxTrack = s.trackId;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
        return;
    }
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
    const StripLayout L = LayoutStrip(x0, h, FxRows(), SendRows());

    if (fDrag == Drag::FxSlot) {
        if (fDragStrip >= (int)fStrips.size()) return;
        // Clamped to the real inserts, so dragging onto the empty or overflow
        // row lands at the end of the chain rather than past it.
        const int nfx = (int)fStrips[fDragStrip].fx.size();
        int to = (int)((where.y - L.fx.top) / kFxRowH);
        if (to < 0) to = 0;
        if (to > nfx - 1) to = nfx - 1;
        if (to != fDragFxTo) { fDragFxTo = to; Invalidate(); }
        return;
    }
    if (fDrag == Drag::Fader) {
        const float fh = L.fader.Height();
        float t = fh > 0 ? (L.fader.bottom - where.y) / fh : 0;
        if (t < 0) t = 0; if (t > 1) t = 1;
        const float gain = t * kMaxGain;
        if (master) fMasterGain = gain;
        else        fStrips[fDragStrip].gain = gain;
    } else if (fDrag == Drag::Trim) {
        // The trim knob: the pan knob's gesture, mapped over the trim scale
        // (unity at 12 o'clock), so a long drag down is silence and up is headroom.
        const float knob = TrimToKnob(fTrimOrig) + (fTrimGrabY - where.y) / 100.0f;
        fStrips[fDragStrip].trim = KnobToTrim(knob);
    } else if (fDrag == Drag::Send) {
        if (fDragStrip >= (int)fStrips.size() || fSendIndex < 0) return;
        MixerStripInfo& s = fStrips[fDragStrip];
        if (fSendIndex >= (int)s.sends.size()) return;
        float v = fSendOrig + (fSendGrabY - where.y) / 100.0f * 1.5f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.5f) v = 1.5f;
        s.sends[(size_t)fSendIndex].level = v;
        // The sends editor's own message: one path into the model, not two.
        PostSendLevel(fDragStrip, fSendIndex, v);
        Invalidate();
        return;   // no ApplyStrip: a send is not part of the strip's own state
    } else {   // Pan knob: vertical drag (up = right) relative to grab.
        float p = fPanOrig + (fPanGrabY - where.y) / 100.0f;
        if (p < -1) p = -1; if (p > 1) p = 1;
        fStrips[fDragStrip].pan = p;
    }
    ApplyStrip(fDragStrip);
    Invalidate();
}

void MixerStripsView::MouseUp(BPoint) {
    const Drag mode = fDrag;
    const int  strip = fDragStrip;
    fDrag = Drag::None;
    fDragStrip = -1;
    fSendIndex = -1;
    if (mode == Drag::Trim || mode == Drag::Send) { Invalidate(); return; }
    if (mode != Drag::FxSlot) return;

    const int from = fDragFxFrom, to = fDragFxTo;
    const uint64 track = fDragFxTrack;
    fDragFxFrom = fDragFxTo = -1;
    fDragFxTrack = 0;
    if (strip < 0 || strip >= (int)fStrips.size()) { Invalidate(); return; }
    const int nfx = (int)fStrips[strip].fx.size();
    if (from < 0 || from >= nfx) { Invalidate(); return; }

    if (to < 0 || to >= nfx || to == from) {
        // Never moved: a click opens the editor on the insert that was clicked.
        BMessage m(kMsgMixFx);
        m.AddInt64("track", (int64)track);
        m.AddInt32("slot", from);
        fApply.SendMessage(&m);
        Invalidate();
        return;
    }
    // The move goes to the main window, which owns the descriptors: the mixer's
    // snapshot has labels, not a chain it could reorder and post back.
    BMessage m(kMsgMixFxMove);
    m.AddInt64("track", (int64)track);
    m.AddInt32("from", from);
    m.AddInt32("to", to);
    fApply.SendMessage(&m);
    Invalidate();
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
    if (ForwardUndoRedo(m, fApply)) return;          // Cmd-Z from a child window
    if (ForwardSpaceToTransport(m, fApply)) return;
    BWindow::DispatchMessage(m, h);
}

void MixerWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgMixerActivate) {   // already open: come to the front
        Activate(true);
        SetWorkspaces(B_CURRENT_WORKSPACE);
        return;
    }
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
    if (msg->what == kMsgMixStrips) {
        std::vector<MixerStripInfo> strips;
        int64 tid = 0;
        // The insert chains and the sends arrive as one flat run each across
        // all strips, sliced by each strip's own count -- a BMessage has no
        // per-strip array to put them in.
        int32 fxIdx = 0, sendIdx = 0;
        for (int32 i = 0; msg->FindInt64("tid", i, &tid) == B_OK; i++) {
            MixerStripInfo s{};
            s.trackId = (uint64)tid;
            const char* nm = nullptr; msg->FindString("nm", i, &nm);
            s.name = nm ? nm : "";
            msg->FindFloat("g", i, &s.gain);
            msg->FindFloat("p", i, &s.pan);
            bool b = false;
            msg->FindBool("mu", i, &b); s.muted = b;
            msg->FindBool("so", i, &b); s.soloed = b;
            msg->FindBool("ar", i, &b); s.armed = b;
            msg->FindBool("mo", i, &b); s.inputMonitor = b;
            int32 v = 0;
            msg->FindInt32("ci", i, &v); s.colorIndex = v;
            msg->FindInt32("ty", i, &v); s.type = v;
            v = 0;
            msg->FindInt32("fx", i, &v);
            for (int32 k = 0; k < v; k++, fxIdx++) {
                MixerInsertInfo ins;
                const char* fn = nullptr;
                msg->FindString("fxn", fxIdx, &fn);
                ins.name = fn ? fn : "";
                bool fb = false;
                msg->FindBool("fxb", fxIdx, &fb);
                ins.bypassed = fb;
                s.fx.push_back(std::move(ins));
            }
            v = 0;
            msg->FindInt32("sn", i, &v);
            for (int32 k = 0; k < v; k++, sendIdx++) {
                MixerSendInfo si;
                const char* sl = nullptr;
                msg->FindString("sdn", sendIdx, &sl);
                si.label = sl ? sl : "";
                msg->FindFloat("slv", sendIdx, &si.level);
                s.sends.push_back(std::move(si));
            }
            // The strip extras (M3.4); an older message without them leaves the
            // defaults, which are the model's own defaults.
            msg->FindFloat("tr", i, &s.trim);
            msg->FindBool("ph", i, &b); s.phaseInvert = b;
            msg->FindInt32("sg", i, &s.soloGroup);
            msg->FindBool("hi", i, &b); s.hasInput = b;
            const char* ol = nullptr; msg->FindString("ol", i, &ol);
            s.outLabel = ol ? ol : "Mst";
            strips.push_back(std::move(s));
        }
        float mg = 1.0f; msg->FindFloat("mg", &mg);
        fView->SetStrips(std::move(strips), mg);
        return;
    }
    BWindow::MessageReceived(msg);
}

} // namespace daw
