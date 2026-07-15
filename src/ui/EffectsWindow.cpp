#include "EffectsWindow.h"

#include "UiMetrics.h"
#include "../dsp/Eq.h"
#include "../plugin/PluginHost.h"

#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

static constexpr float kPanelPad = 8.0f;
static constexpr float kTitleH   = 22.0f;
static constexpr float kKnobW    = 68.0f;
static constexpr float kKnobH    = 86.0f;
static constexpr float kGraphH   = 156.0f;
static constexpr float kSelH     = 26.0f;    // reverb type-selector row
static constexpr float kBtnW     = 22.0f;

struct KnobDef { const char* label; int slot; float mn; float mx; };

static const char* EffName(EffectType t) {
    switch (t) {
        case EffectType::Delay:      return "Delay";
        case EffectType::Reverb:     return "Reverb";
        case EffectType::Compressor: return "Compressor";
        case EffectType::Eq:         return "EQ (5-band)";
        case EffectType::Saturator:  return "Saturator";
        case EffectType::Gate:       return "Gate";
        case EffectType::Widener:    return "Widener";
        default:                     return "Biquad";
    }
}

// Knobs shown for each effect. EQ uses the graph for freq/gain; only the 5 Q
// knobs are listed here.
static std::vector<KnobDef> KnobsFor(EffectType t) {
    switch (t) {
        case EffectType::Delay:  return {{"Time", 0, 0.01f, 1.0f}, {"Fbk", 1, 0, 0.95f}, {"Mix", 2, 0, 1}};
        case EffectType::Reverb: return {{"Size", 0, 0, 1}, {"Mix", 1, 0, 1},
                                         {"Decay", 3, 0.2f, 12}, {"Tone", 4, 0, 1}};
        case EffectType::Compressor:
            return {{"Thr dB", 0, -60, 0}, {"Ratio", 1, 1, 20}, {"Atk ms", 2, 0.1f, 100},
                    {"Rel ms", 3, 5, 1000}, {"Makeup", 4, 0, 24}};
        case EffectType::Saturator: return {{"Drive", 0, 0, 1}, {"Mix", 1, 0, 1}, {"Out dB", 2, -24, 24}};
        case EffectType::Gate:
            return {{"Thr dB", 0, -80, 0}, {"Ratio", 1, 1, 20}, {"Atk ms", 2, 0.1f, 100},
                    {"Rel ms", 3, 5, 1000}, {"Range", 4, 0, 80}};
        case EffectType::Widener: return {{"Width", 0, 0, 2}, {"Pan", 1, -1, 1}, {"Gain", 2, 0, 2}};
        case EffectType::Eq:
            return {{"Low Q", 2, 0.3f, 8}, {"LoM Q", 5, 0.3f, 8}, {"Mid Q", 8, 0.3f, 8},
                    {"HiM Q", 11, 0.3f, 8}, {"Hi Q", 14, 0.3f, 8}};
        case EffectType::Biquad:
        default: return {{"Freq", 1, 20, 16000}, {"Q", 2, 0.1f, 10}};
    }
}

static const PluginInfo* FindPlugin(const std::string& name) {
    for (const PluginInfo& pi : PluginHost::Instance().Plugins())
        if (pi.name == name) return &pi;
    return nullptr;
}

// Knob row for a descriptor. Plugins derive their knobs (label + range) from
// the loaded add-on's parameter metadata; built-ins use the static table.
static std::vector<KnobDef> KnobsForDesc(const EffectDesc& d) {
    if (d.type == EffectType::Plugin) {
        std::vector<KnobDef> ks;
        if (const PluginInfo* pi = FindPlugin(d.pluginName))
            for (size_t i = 0; i < pi->params.size() && i < 5; i++)
                ks.push_back({ pi->params[i].name.c_str(), (int)i,
                               pi->params[i].mn, pi->params[i].mx });
        return ks;
    }
    return KnobsFor(d.type);
}

EffectsView::EffectsView(BRect frame, std::vector<EffectDesc> chain,
                         TrackId track, BMessenger apply)
    : BView(frame, "fx", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW),
      fChain(std::move(chain)), fTrack(track), fApply(apply) {
    SetViewColor(ColBackground());
}

void EffectsView::SetMeters(const float* gr, int grN,
                            const float* spec, int specN, int specFx) {
    fGrN = grN < 16 ? grN : 16;
    for (int i = 0; i < fGrN; i++) fGr[i] = gr[i];
    fSpecN = specN < kSpecMax ? specN : kSpecMax;
    if (spec) for (int i = 0; i < fSpecN; i++) fSpec[i] = spec[i];
    fSpecFx = specFx;
    Invalidate();
}

float EffectsView::PanelHeight(const EffectDesc& d) const {
    float h = kTitleH + 6;
    if (d.type == EffectType::Eq || d.type == EffectType::Compressor)
        h += kGraphH;
    if (d.type == EffectType::Reverb)
        h += kSelH;   // reverb type selector row
    h += kKnobH;   // one knob row (all effects have <= 5 knobs)
    return h + 8;
}

float EffectsView::PanelTop(size_t i) const {
    float y = kPanelPad;
    for (size_t k = 0; k < i && k < fChain.size(); k++)
        y += PanelHeight(fChain[k]) + 6;
    return y;
}

float EffectsView::ContentHeight() const {
    float y = kPanelPad;
    for (const EffectDesc& d : fChain) y += PanelHeight(d) + 6;
    // 7 built-in add buttons + one per loaded plugin.
    const int addRows = 7 + (int)PluginHost::Instance().Plugins().size();
    y += addRows * 26 + 12;
    return y;
}

void EffectsView::Apply() {
    BMessage m(kMsgApplyFx);
    m.AddInt64("track", (int64)fTrack);
    for (const EffectDesc& d : fChain) {
        m.AddInt32("et", (int32)(int)d.type);
        m.AddInt32("ec", (int32)d.params.size());
        m.AddString("en", d.pluginName.c_str());   // empty for built-ins
        for (float v : d.params) m.AddFloat("ep", v);
    }
    fApply.SendMessage(&m);
}

// --- drawing --------------------------------------------------------------

void EffectsView::DrawKnob(BRect r, const char* label, float value,
                           float mn, float mx) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = r.top + 42;      // pushed down so the dial clears the label
    const float rad = 20.0f;
    float t = (mx > mn) ? (value - mn) / (mx - mn) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;

    // Label (centered above, clear of the dial + its tick ring).
    SetHighColor(ColText());
    DrawString(label, BPoint(cx - StringWidth(label) * 0.5f, r.top + 10));

    // Tick scale around the -135..+135 sweep; the reached ticks are lit.
    for (int i = 0; i <= 10; i++) {
        const double tf = i / 10.0;
        const double a  = (-135.0 + 270.0 * tf) * M_PI / 180.0;
        const float ux = (float)std::sin(a), uy = -(float)std::cos(a);
        SetHighColor(tf <= t + 0.001 ? ColAccent() : ColGrid());
        StrokeLine(BPoint(cx + ux * (rad + 2), cy + uy * (rad + 2)),
                   BPoint(cx + ux * (rad + 6), cy + uy * (rad + 6)));
    }
    // Body.
    SetHighColor(ColHeaderHi());
    FillEllipse(BPoint(cx, cy), rad, rad);
    SetHighColor(ColGrid());
    StrokeEllipse(BPoint(cx, cy), rad, rad);
    // Indicator (thick).
    const double ang = (-135.0 + 270.0 * t) * M_PI / 180.0;
    const float ix = (float)std::sin(ang), iy = -(float)std::cos(ang);
    SetHighColor(ColAccent());
    SetPenSize(2.0f);
    StrokeLine(BPoint(cx + ix * 5, cy + iy * 5),
               BPoint(cx + ix * (rad - 3), cy + iy * (rad - 3)));
    SetPenSize(1.0f);

    // Value (centered below).
    char v[16];
    if (std::fabs(mx) > 50 || std::fabs(mn) > 50) std::snprintf(v, sizeof(v), "%.0f", value);
    else std::snprintf(v, sizeof(v), "%.2f", value);
    SetHighColor(ColText());
    DrawString(v, BPoint(cx - StringWidth(v) * 0.5f, r.bottom - 3));
}

void EffectsView::DrawEqGraph(BRect r, const EffectDesc& d, int effIdx) {
    SetHighColor(Rgb(16, 18, 22));
    FillRect(r);
    // dB grid (-18..+18) + 0 line.
    for (int db = -18; db <= 18; db += 6) {
        const float y = r.bottom - (db + 18) / 36.0f * r.Height();
        SetHighColor(db == 0 ? ColGrid() : ColLaneAlt());
        StrokeLine(BPoint(r.left, y), BPoint(r.right, y));
    }
    // Freq grid (log, 20..20000; decades at 100/1000/10000).
    auto freqToX = [&](double f) {
        const double lo = std::log10(20.0), hi = std::log10(20000.0);
        double t = (std::log10(f) - lo) / (hi - lo);
        return r.left + (float)t * r.Width();
    };
    auto xToFreq = [&](float x) {
        const double lo = std::log10(20.0), hi = std::log10(20000.0);
        double t = (x - r.left) / r.Width();
        return std::pow(10.0, lo + t * (hi - lo));
    };
    for (double f : { 100.0, 1000.0, 10000.0 }) {
        const float x = freqToX(f);
        SetHighColor(ColLaneAlt());
        StrokeLine(BPoint(x, r.top), BPoint(x, r.bottom));
    }

    // Live FFT analyzer overlay (input spectrum), drawn behind the EQ curve.
    if (fFftOn && fSpecFx == effIdx && fSpecN > 1) {
        const double binHz = 48000.0 / (double)Eq::kFftSize;
        auto specY = [&](float db) {
            if (db < -84) db = -84; if (db > 0) db = 0;
            return r.bottom - (db + 84.0f) / 84.0f * r.Height();
        };
        SetHighColor(Rgb(46, 96, 86));   // dim teal spectrum fill
        for (float x = r.left + 1; x <= r.right; x += 1.0f) {
            const double f = xToFreq(x);
            const int k = (int)(f / binHz + 0.5);
            if (k < 1 || k >= fSpecN) continue;
            StrokeLine(BPoint(x, r.bottom), BPoint(x, specY(fSpec[k])));
        }
    }

    // Build an Eq from the params and stroke its magnitude response.
    Eq eq;
    for (int b = 0; b < 5; b++)
        eq.SetBand(b, d.p((size_t)(b * 3)), d.p((size_t)(b * 3 + 1)),
                   d.p((size_t)(b * 3 + 2)));
    eq.Prepare(48000.0);
    auto dbToY = [&](float db) {
        if (db < -18) db = -18; if (db > 18) db = 18;
        return r.bottom - (db + 18) / 36.0f * r.Height();
    };
    SetHighColor(ColAccent());
    float px = r.left, py = dbToY(eq.MagnitudeResponseDb((float)xToFreq(r.left)));
    for (float x = r.left + 2; x <= r.right; x += 2) {
        const float y = dbToY(eq.MagnitudeResponseDb((float)xToFreq(x)));
        StrokeLine(BPoint(px, py), BPoint(x, y));
        px = x; py = y;
    }

    // Draggable band handles (x = freq, y = gain).
    for (int b = 0; b < 5; b++) {
        const float f = d.p((size_t)(b * 3));
        const float g = d.p((size_t)(b * 3 + 1));
        const float hx = freqToX(f <= 20 ? 20 : f);
        const float hy = dbToY(g);
        SetHighColor(TrackColor(b));
        FillEllipse(BPoint(hx, hy), 5, 5);
        SetHighColor(ColText());
        StrokeEllipse(BPoint(hx, hy), 5, 5);
        fHits.push_back({ effIdx, 5, b, BRect(hx - 6, hy - 6, hx + 6, hy + 6),
                          0, 0 });
    }

    // Axis labels last (on top of the curve/handles) so they stay legible.
    SetHighColor(ColTextDim());
    for (double f : { 100.0, 1000.0, 10000.0 })
        DrawString(f >= 1000 ? (f >= 10000 ? "10k" : "1k") : "100",
                   BPoint(freqToX(f) + 2, r.bottom - 2));
    for (int db = 12; db >= -12; db -= 12) {
        const float y = r.bottom - (db + 18) / 36.0f * r.Height();
        char l[8]; std::snprintf(l, sizeof(l), "%+d", db);
        DrawString(l, BPoint(r.left + 2, y - 2));
    }
    SetHighColor(ColGrid());
    StrokeRect(r);
}

void EffectsView::DrawCompCurve(BRect r, const EffectDesc& d, int effIdx) {
    SetHighColor(Rgb(16, 18, 22));
    FillRect(r);
    // Gain-reduction meter strip down the right edge (live), 0..-24 dB.
    const float grDb = (effIdx >= 0 && effIdx < fGrN) ? -fGr[effIdx] : 0.0f;  // >=0
    const float meterW = 14.0f;
    BRect gm(r.right - meterW, r.top, r.right, r.bottom);
    SetHighColor(Rgb(12, 14, 17));
    FillRect(gm);
    const float grN = grDb / 24.0f > 1.0f ? 1.0f : grDb / 24.0f;   // fraction
    SetHighColor(Rgb(232, 150, 70));
    FillRect(BRect(gm.left + 2, gm.top, gm.right - 1, gm.top + grN * gm.Height()));
    SetHighColor(ColGrid());
    StrokeRect(gm);
    SetHighColor(ColTextDim());
    DrawString("GR", BPoint(gm.left - 1, r.top + 10));
    char grl[16]; std::snprintf(grl, sizeof(grl), "-%.1f", grDb);
    DrawString(grl, BPoint(gm.left - StringWidth(grl) - 3, r.bottom - 3));
    r.right -= meterW + 2;   // curve area excludes the meter

    const float thr = d.p(0);     // dB
    const float ratio = d.p(1) < 1 ? 1 : d.p(1);
    const float makeup = d.p(4);
    // Axes: input/output -60..0 dB.
    auto mapx = [&](float in) { return r.left + (in + 60) / 60.0f * r.Width(); };
    auto mapy = [&](float out) {
        if (out > 6) out = 6; if (out < -60) out = -60;
        return r.bottom - (out + 60) / 66.0f * r.Height();
    };
    // Unity reference.
    SetHighColor(ColLaneAlt());
    StrokeLine(BPoint(mapx(-60), mapy(-60)), BPoint(mapx(0), mapy(0)));
    // Transfer curve.
    SetHighColor(ColAccent());
    float px = mapx(-60), py = mapy(-60 + makeup);
    for (float in = -60; in <= 0; in += 1.0f) {
        float out = (in <= thr) ? in : thr + (in - thr) / ratio;
        out += makeup;
        const float x = mapx(in), y = mapy(out);
        StrokeLine(BPoint(px, py), BPoint(x, y));
        px = x; py = y;
    }
    // Threshold marker.
    SetHighColor(ColPlayhead());
    StrokeLine(BPoint(mapx(thr), r.top), BPoint(mapx(thr), r.bottom));
    SetHighColor(ColGrid());
    StrokeRect(r);
}

void EffectsView::Draw(BRect) {
    fHits.clear();
    const float w = Bounds().Width();

    for (size_t i = 0; i < fChain.size(); i++) {
        const EffectDesc& d = fChain[i];
        const float top = PanelTop(i);
        const float ph  = PanelHeight(d);
        BRect panel(kPanelPad, top, w - kPanelPad, top + ph - 6);
        SetHighColor(ColHeader());
        FillRect(panel);
        SetHighColor(ColHeaderHi());
        FillRect(BRect(panel.left, panel.top, panel.right, panel.top + kTitleH));
        SetHighColor(ColText());
        const char* title = EffName(d.type);
        if (d.type == EffectType::Plugin) title = d.pluginName.c_str();
        else if (d.type == EffectType::Reverb) {
            const int algo = (int)(d.p(2) + 0.5f);
            title = algo == 1 ? "Reverb - Plate"
                  : algo == 2 ? "Reverb - Hall"
                  : algo == 3 ? "Reverb - FDN" : "Reverb";
        }
        DrawString(title, BPoint(panel.left + 8, panel.top + 15));

        // Up / Down / Remove buttons in the title bar.
        auto btn = [&](float rx, const char* lbl, int kind) {
            BRect b(rx, panel.top + 2, rx + kBtnW, panel.top + kTitleH - 2);
            SetHighColor(ColHeaderHi());  FillRect(b);
            SetHighColor(ColGrid());      StrokeRect(b);
            SetHighColor(ColText());      DrawString(lbl, BPoint(b.left + 6, b.bottom - 5));
            fHits.push_back({ (int)i, kind, 0, b, 0, 0 });
        };
        btn(panel.right - 3 * kBtnW - 60, "^", 1);
        btn(panel.right - 2 * kBtnW - 56, "v", 2);
        BRect rm(panel.right - 52, panel.top + 2, panel.right - 4, panel.top + kTitleH - 2);
        SetHighColor(Rgb(120, 60, 60)); FillRect(rm);
        SetHighColor(ColGrid());        StrokeRect(rm);
        SetHighColor(ColText());        DrawString("Del", BPoint(rm.left + 12, rm.bottom - 5));
        fHits.push_back({ (int)i, 3, 0, rm, 0, 0 });

        // EQ analyzer (FFT) on/off toggle in the title bar.
        if (d.type == EffectType::Eq) {
            BRect fb(panel.left + 96, panel.top + 3, panel.left + 132,
                     panel.top + kTitleH - 3);
            SetHighColor(fFftOn ? ColAccent() : ColHeaderHi());
            FillRect(fb);
            SetHighColor(ColGrid()); StrokeRect(fb);
            SetHighColor(fFftOn ? ColBackground() : ColTextDim());
            DrawString("FFT", BPoint(fb.left + 8, fb.bottom - 5));
            fHits.push_back({ (int)i, 6, 0, fb, 0, 0 });   // kind 6 = FFT toggle
        }

        float knobTop = panel.top + kTitleH + 4;
        if (d.type == EffectType::Eq) {
            DrawEqGraph(BRect(panel.left + 6, knobTop, panel.right - 6,
                              knobTop + kGraphH - 6), d, (int)i);
            knobTop += kGraphH;
        } else if (d.type == EffectType::Compressor) {
            DrawCompCurve(BRect(panel.left + 6, knobTop, panel.right - 6,
                                knobTop + kGraphH - 6), d, (int)i);
            knobTop += kGraphH;
        } else if (d.type == EffectType::Reverb) {
            // Reverb type dropdown (Classic / Plate / Hall / FDN).
            const int algo = (int)(d.p(2) + 0.5f);
            const char* nm = algo == 1 ? "Plate" : algo == 2 ? "Hall"
                           : algo == 3 ? "FDN" : "Classic";
            BRect sel(panel.left + 6, knobTop + 2, panel.right - 6, knobTop + 22);
            SetHighColor(ColHeaderHi()); FillRect(sel);
            SetHighColor(ColGrid());     StrokeRect(sel);
            SetHighColor(ColTextDim());
            DrawString("Type:", BPoint(sel.left + 8, sel.bottom - 6));
            SetHighColor(ColAccent());
            DrawString(nm, BPoint(sel.left + 52, sel.bottom - 6));
            SetHighColor(ColText());
            DrawString("v", BPoint(sel.right - 14, sel.bottom - 6));   // dropdown arrow
            fHits.push_back({ (int)i, 7, 0, sel, 0, 0 });   // kind 7 = reverb type
            knobTop += kSelH;
        }

        // Knob row.
        const std::vector<KnobDef> knobs = KnobsForDesc(d);
        float kx = panel.left + 8;
        for (const KnobDef& k : knobs) {
            BRect kr(kx, knobTop, kx + kKnobW, knobTop + kKnobH);
            DrawKnob(kr, k.label, d.p((size_t)k.slot), k.mn, k.mx);
            fHits.push_back({ (int)i, 0, k.slot, kr, k.mn, k.mx });
            kx += kKnobW + 4;
        }
    }

    // Add-effect buttons.
    float ay = PanelTop(fChain.size());
    const char* adds[8] = { "Add EQ", "Add Delay", "Add Reverb", "Add Compressor",
                            "Add Saturator", "Add Gate", "Add Widener", "" };
    const EffectType at[7] = { EffectType::Eq, EffectType::Delay, EffectType::Reverb,
                               EffectType::Compressor, EffectType::Saturator,
                               EffectType::Gate, EffectType::Widener };
    for (int k = 0; k < 7; k++) {
        BRect b(kPanelPad, ay, w - kPanelPad, ay + 22);
        SetHighColor(ColHeaderHi()); FillRect(b);
        SetHighColor(ColGrid());     StrokeRect(b);
        SetHighColor(ColAccent());   DrawString(adds[k], BPoint(b.left + 10, b.bottom - 6));
        fHits.push_back({ (int)at[k], 4, 0, b, 0, 0 });
        ay += 26;
    }
    // One button per loaded plugin add-on (slot carries the plugin index).
    const std::vector<PluginInfo>& plugins = PluginHost::Instance().Plugins();
    for (size_t k = 0; k < plugins.size(); k++) {
        BRect b(kPanelPad, ay, w - kPanelPad, ay + 22);
        SetHighColor(ColHeaderHi()); FillRect(b);
        SetHighColor(ColGrid());     StrokeRect(b);
        SetHighColor(ColAccent());
        std::string lbl = "Add " + plugins[k].name;
        DrawString(lbl.c_str(), BPoint(b.left + 10, b.bottom - 6));
        fHits.push_back({ (int)EffectType::Plugin, 4, (int)k, b, 0, 0 });
        ay += 26;
    }
}

// --- interaction ----------------------------------------------------------

int EffectsView::HitTest(BPoint where, Hit* out) const {
    for (const Hit& h : fHits)
        if (h.rect.Contains(where)) { *out = h; return h.kind; }
    return -1;
}

static EffectDesc MakeDefault(EffectType t) {
    switch (t) {
        case EffectType::Delay:      return DelayDesc();
        case EffectType::Reverb:     return DuskPlateDesc();   // type via dropdown
        case EffectType::Compressor: return CompressorDesc();
        case EffectType::Saturator:  return SaturatorDesc();
        case EffectType::Gate:       return GateDesc();
        case EffectType::Widener:    return WidenerDesc();
        case EffectType::Eq:
        default:                     return EqDesc();
    }
}

void EffectsView::MouseDown(BPoint where) {
    Hit h;
    const int kind = HitTest(where, &h);
    if (kind < 0) return;

    // Right-click a knob toggles automation of that parameter (per-track only;
    // the master chain has no per-track automation lanes).
    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    if ((buttons & B_SECONDARY_MOUSE_BUTTON) && kind == 0
        && h.effect >= 0 && h.effect < (int)fChain.size()) {
        BMessage m(kMsgToggleFxAuto);
        m.AddInt64("track", (int64)fTrack);
        m.AddInt32("fx", h.effect);
        m.AddInt32("slot", h.slot);
        m.AddFloat("val", fChain[h.effect].p((size_t)h.slot));
        fApply.SendMessage(&m);
        return;
    }

    switch (kind) {
        case 0:   // knob: begin a vertical drag
        case 5: { // eq handle: 2D drag
            fDragEffect = h.effect; fDragSlot = h.slot; fDragKind = kind;
            fDragMin = h.min; fDragMax = h.max;
            fDragStart = where;
            fDragStartVal = (fDragEffect >= 0 && fDragEffect < (int)fChain.size())
                            ? fChain[fDragEffect].p((size_t)fDragSlot) : 0;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            break;
        }
        case 1: case 2: {   // up / down reorder
            const int j = h.effect + (kind == 1 ? -1 : 1);
            if (h.effect >= 0 && h.effect < (int)fChain.size()
                && j >= 0 && j < (int)fChain.size()) {
                std::swap(fChain[h.effect], fChain[j]);
                Apply(); Invalidate();
            }
            break;
        }
        case 6:   // FFT analyzer on/off
            fFftOn = !fFftOn;
            Invalidate();
            break;
        case 7: {   // reverb type dropdown
            if (h.effect < 0 || h.effect >= (int)fChain.size()) break;
            BPopUpMenu* menu = new BPopUpMenu("type", false, false);
            const char* names[4] = { "Classic", "Plate", "Hall",
                                     "FDN" };
            EffectDesc& d = fChain[h.effect];
            const int cur = (int)(d.p(2) + 0.5f);
            for (int a = 0; a < 4; a++) {
                BMenuItem* it = new BMenuItem(names[a], nullptr);
                if (a == cur) it->SetMarked(true);
                menu->AddItem(it);
            }
            BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
            if (sel) {
                const int a = menu->IndexOf(sel);
                if (d.params.size() < 5) d.params.resize(5, 0.0f);
                if (d.params[3] <= 0.0f) d.params[3] = 2.6f;  // decay default
                d.params[2] = (float)a;
                Apply(); Invalidate();
            }
            delete menu;
            break;
        }
        case 3:   // remove
            if (h.effect >= 0 && h.effect < (int)fChain.size()) {
                fChain.erase(fChain.begin() + h.effect);
                Apply(); Invalidate();
            }
            break;
        case 4:   // add (h.effect carries the EffectType; slot = plugin index)
            if ((EffectType)h.effect == EffectType::Plugin) {
                const std::vector<PluginInfo>& pl =
                    PluginHost::Instance().Plugins();
                if (h.slot >= 0 && h.slot < (int)pl.size()) {
                    EffectDesc d;
                    d.type = EffectType::Plugin;
                    d.pluginName = pl[h.slot].name;
                    for (const PluginParamInfo& pp : pl[h.slot].params)
                        d.params.push_back(pp.def);   // seed defaults
                    fChain.push_back(d);
                    Apply(); Invalidate();
                }
            } else {
                fChain.push_back(MakeDefault((EffectType)h.effect));
                Apply(); Invalidate();
            }
            break;
    }
}

void EffectsView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDragEffect < 0 || fDragEffect >= (int)fChain.size()) return;
    EffectDesc& d = fChain[fDragEffect];
    auto setP = [&](int slot, float v) {
        if ((int)d.params.size() <= slot) d.params.resize(slot + 1, 0.0f);
        d.params[slot] = v;
    };
    if (fDragKind == 0) {   // knob: vertical drag over ~160 px = full range
        const float dv = (fDragStart.y - where.y) / 160.0f * (fDragMax - fDragMin);
        float v = fDragStartVal + dv;
        if (v < fDragMin) v = fDragMin; if (v > fDragMax) v = fDragMax;
        setP(fDragSlot, v);
    } else if (fDragKind == 5) {   // eq handle: x -> freq (log), y -> gain
        // Recover the graph rect for this effect to map coordinates.
        const float top = PanelTop((size_t)fDragEffect) + kTitleH + 4;
        BRect r(kPanelPad + 6, top, Bounds().Width() - kPanelPad - 6, top + kGraphH - 6);
        const double lo = std::log10(20.0), hi = std::log10(20000.0);
        double tf = (where.x - r.left) / r.Width();
        if (tf < 0) tf = 0; if (tf > 1) tf = 1;
        const float freq = (float)std::pow(10.0, lo + tf * (hi - lo));
        float gain = 18.0f - (where.y - r.top) / r.Height() * 36.0f;
        if (gain < -18) gain = -18; if (gain > 18) gain = 18;
        setP(fDragSlot * 3, freq);
        setP(fDragSlot * 3 + 1, gain);
    }
    Invalidate();   // live preview; commit (one undo step) on mouse-up
}

void EffectsView::MouseUp(BPoint) {
    if (fDragEffect >= 0) Apply();   // commit the drag as one undoable change
    fDragEffect = fDragSlot = fDragKind = -1;
}

// --- window ---------------------------------------------------------------

EffectsWindow::EffectsWindow(BRect frame, std::vector<EffectDesc> chain,
                             TrackId track, BMessenger apply)
    : BWindow(frame, "Effects", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fTrack(track), fApply(apply) {
    BRect b = Bounds();
    BRect vr(b.left, b.top, b.right - B_V_SCROLL_BAR_WIDTH, b.bottom);
    fView = new EffectsView(vr, std::move(chain), track, apply);
    BScrollView* sv = new BScrollView("sv", fView, B_FOLLOW_ALL_SIDES, 0,
                                      false, true);
    AddChild(sv);
    if (BScrollBar* bar = sv->ScrollBar(B_VERTICAL)) {
        const float ch = fView->ContentHeight();
        bar->SetRange(0, std::max(0.0f, ch - vr.Height()));
        bar->SetSteps(16, vr.Height());
    }
    // Tell the main window to meter this track's effects while we're open.
    BMessage open(kMsgFxWinOpen);
    open.AddInt64("track", (int64)track);
    open.AddMessenger("msgr", BMessenger(this));
    fApply.SendMessage(&open);
}

void EffectsWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgFxMeter) {
        const float* gr = nullptr; ssize_t grBytes = 0;
        const float* sp = nullptr; ssize_t spBytes = 0;
        int32 specFx = -1, specN = 0;
        msg->FindData("gr", B_FLOAT_TYPE, (const void**)&gr, &grBytes);
        msg->FindData("spec", B_FLOAT_TYPE, (const void**)&sp, &spBytes);
        msg->FindInt32("specfx", &specFx);
        msg->FindInt32("specn", &specN);
        fView->SetMeters(gr, gr ? (int)(grBytes / sizeof(float)) : 0,
                         sp, specN, specFx);
        return;
    }
    BWindow::MessageReceived(msg);
}

bool EffectsWindow::QuitRequested() {
    BMessage closed(kMsgFxWinClosed);
    closed.AddInt64("track", (int64)fTrack);
    fApply.SendMessage(&closed);
    return true;
}

} // namespace daw
