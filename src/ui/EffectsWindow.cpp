#include "EffectsWindow.h"

#include "UiMetrics.h"
#include "../dsp/Eq.h"
#include "../dsp/Delay.h"   // Delay::DivisionName / kDivisionCount (sync selector)
#include "../plugin/PluginHost.h"
#include "../plugin/Lv2PortMap.h"   // ClampLv2Param: one definition of a port's domain
#include "PluginBrowser.h"

// Only linked when CMake found lilv; DAW_HAVE_LV2 comes from the daw_lv2 target.
// Without it every LV2 branch below compiles out and an Lv2 insert simply shows
// no knobs, the same as an add-on that isn't installed.
#ifdef DAW_HAVE_LV2
#include "../plugin/Lv2Host.h"
#include "Lv2UiWindow.h"
#endif

#include <MenuItem.h>
#include <String.h>
#include <MessageRunner.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace daw {

// Self-addressed: fires once a wheel gesture has gone quiet (see ScheduleCommit).
static constexpr uint32 kMsgFxCommit = 'fxcm';
// A wheel notch moves 1/40 of a parameter's range (1/200 with Shift, for trim).
static constexpr float  kWheelCoarse = 40.0f;
static constexpr float  kWheelFine   = 200.0f;
// How long the wheel must sit still before the edit becomes one undo step.
static constexpr bigtime_t kWheelCommitDelay = 400000;   // 400 ms

static constexpr float kPanelPad = 8.0f;
static constexpr float kTitleH   = 22.0f;
static constexpr float kKnobW    = 68.0f;
static constexpr float kKnobH    = 86.0f;
// Dial geometry inside a knob cell (see DrawKnob): centre offset from the cell
// top, body radius, and how far the tick ring extends past it.
static constexpr float kDialCy   = 42.0f;
static constexpr float kDialR    = 20.0f;
static constexpr float kDialTick = 6.0f;
static constexpr float kGraphH   = 156.0f;
static constexpr float kSelH     = 26.0f;    // reverb type-selector row
static constexpr float kBtnW     = 22.0f;
// Generic plugin parameter list: one horizontal slider row per parameter. A
// knob row cannot serve here -- it is capped at the five that fit across the
// panel, and a plugin can expose far more (4K EQ 2 has 26). Vertical rows scale
// to any count and give the label room to be read.
static constexpr float kParamRowH   = 24.0f;
static constexpr float kParamLabelW = 124.0f;
static constexpr float kParamValW   = 64.0f;

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
        case EffectType::Limiter:    return "Limiter";
        case EffectType::Lv2:        return "LV2";
        default:                     return "Biquad";
    }
}

// Public: the label the channel strip and the panel header both show.
std::string EffectDisplayName(const EffectDesc& d) {
    if (d.type == EffectType::Plugin)
        return d.pluginName.empty() ? "Plugin" : d.pluginName;
#ifdef DAW_HAVE_LV2
    if (d.type == EffectType::Lv2) {
        // pluginName is the URI for LV2 -- unreadable as a label -- so ask the
        // host for the display name it read from the bundle. An unresolved URI
        // falls through to the bare type, which is what an uninstalled plugin
        // shows.
        if (const Lv2PluginInfo* pi = Lv2Host::Instance().Find(d.pluginName))
            if (!pi->name.empty()) return pi->name;
    }
#endif
    if (d.type == EffectType::Reverb) {
        const int algo = (int)(d.p(2) + 0.5f);
        if (algo == 1) return "Reverb - Plate";
        if (algo == 2) return "Reverb - Hall";
        if (algo == 3) return "Reverb - FDN";
    }
    return EffName(d.type);
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
        case EffectType::Limiter:
            return {{"Ceil dB", 0, -24, 0}, {"Look ms", 1, 0.1f, 20}, {"Rel ms", 2, 1, 2000},
                    {"In dB", 3, 0, 24}};
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

// How many of a plugin's parameters fit on the single knob row. Built-ins top
// out at 5 (Compressor, Gate), so the row is already sized for it. A plugin with
// more than this shows its first few; the generic vertical parameter panel is
// what will eventually show them all.
static constexpr size_t kMaxPluginKnobs = 5;

// One effect parameter as described by whichever host owns the plugin.
//
// PluginParamInfo (add-ons) and Lv2ParamInfo (LV2) deliberately carry the same
// field names, so this struct is the ONLY place that has to know which host a
// descriptor came from. `name` points into the host's own listing, which is
// built once at startup and never mutated afterwards — the same lifetime the
// add-on knob labels already relied on.
struct HostParam { const char* name; float mn; float mx; float def;
                   bool isInteger; bool isToggled;
                   // Points into the host's listing, which is built once at
                   // startup and never mutated; null for non-enumerations.
                   const std::vector<float>* scalePoints; };

static std::vector<HostParam> HostParamsFor(const EffectDesc& d) {
    std::vector<HostParam> out;
    if (d.type == EffectType::Plugin) {
        if (const PluginInfo* pi = FindPlugin(d.pluginName))
            for (const PluginParamInfo& p : pi->params)
                // The add-on ABI does not describe integer/toggled/enum ports,
                // so a native plugin's params are always treated as continuous.
                out.push_back({ p.name.c_str(), p.mn, p.mx, p.def,
                                false, false, nullptr });
    }
#ifdef DAW_HAVE_LV2
    else if (d.type == EffectType::Lv2) {
        if (const Lv2PluginInfo* pi = Lv2Host::Instance().Find(d.pluginName))
            for (const Lv2ParamInfo& p : pi->params)
                out.push_back({ p.name.c_str(), p.mn, p.mx, p.def, p.isInteger,
                                p.isToggled, &p.scalePoints });
    }
#endif
    return out;
}

// Number of parameters the owning host describes. Split from HostParamsFor so
// the layout maths can ask "how many rows?" without building and throwing away a
// vector of strings on every panel, for every panel, on every redraw.
static size_t HostParamCount(const EffectDesc& d) {
    if (d.type == EffectType::Plugin) {
        if (const PluginInfo* pi = FindPlugin(d.pluginName)) return pi->params.size();
    }
#ifdef DAW_HAVE_LV2
    else if (d.type == EffectType::Lv2) {
        if (const Lv2PluginInfo* pi = Lv2Host::Instance().Find(d.pluginName))
            return pi->params.size();
    }
#endif
    return 0;
}

// True when this effect is drawn as a vertical parameter list rather than the
// fixed knob row: its parameters are described by a host, not by our table.
static bool UsesParamList(const EffectDesc& d) {
    return d.type == EffectType::Plugin || d.type == EffectType::Lv2;
}

// Grow `d.params` so `slot` is addressable, giving any slot we have to invent a
// value the plugin's OWN port default rather than zero.
//
// Zero is not a neutral choice. Every LV2 plugin available here exposes an
// `Enabled` control whose default is 1, so zero-filling to reach a later slot
// switches the plugin off and it renders silence — which looks exactly like a
// broken host. Built-ins have no host metadata and keep the old zero fill, which
// matches their documented param layouts.
static void EnsureParamSlot(EffectDesc& d, int slot) {
    if (slot < 0 || (int)d.params.size() > slot) return;
    const std::vector<HostParam> hp = HostParamsFor(d);
    const size_t oldSize = d.params.size();
    d.params.resize((size_t)slot + 1, 0.0f);
    for (size_t i = oldSize; i < d.params.size() && i < hp.size(); i++)
        d.params[i] = hp[i].def;
}

// Knob row for a descriptor. Plugin- and LV2-backed effects derive their knobs
// (label + range) from their host's parameter metadata; built-ins use the static
// table. Falling through to that table for a plugin would be actively harmful:
// its default is the Biquad row, whose knobs write Hz-scale values into whatever
// ports happen to sit at slots 1 and 2.
static std::vector<KnobDef> KnobsForDesc(const EffectDesc& d) {
    if (d.type == EffectType::Plugin || d.type == EffectType::Lv2) {
        std::vector<KnobDef> ks;
        const std::vector<HostParam> hp = HostParamsFor(d);
        for (size_t i = 0; i < hp.size() && i < kMaxPluginKnobs; i++)
            ks.push_back({ hp[i].name, (int)i, hp[i].mn, hp[i].mx });
        // Empty when the host has no metadata for this id — an add-on that
        // isn't installed, an LV2 URI that didn't resolve, or a build with no
        // LV2 support at all. That is already a supported, harmless state.
        return ks;
    }
    return KnobsFor(d.type);
}

EffectsView::EffectsView(BRect frame, std::vector<EffectDesc> chain,
                         TrackId track, BMessenger apply, int focusSlot)
    : BView(frame, "fx", B_FOLLOW_LEFT_RIGHT | B_FOLLOW_TOP, B_WILL_DRAW),
      fChain(std::move(chain)), fTrack(track), fApply(apply),
      fFocus(focusSlot >= 0 && focusSlot < (int)fChain.size() ? focusSlot : -1) {
    SetViewColor(ColBackground());
}

EffectsView::~EffectsView() {
    delete fCommit;
}

void EffectsView::FlushPendingEdit() {
    if (!fCommit) return;
    delete fCommit; fCommit = nullptr;
    Apply();
}

void EffectsView::UpdateScrollRange() {
    if (BScrollView* sv = dynamic_cast<BScrollView*>(Parent()))
        if (BScrollBar* bar = sv->ScrollBar(B_VERTICAL)) {
            const float vh = Bounds().Height();
            bar->SetRange(0, std::max(0.0f, ContentHeight() - vh));
            bar->SetSteps(16, vh);
        }
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
    if (UsesParamList(d)) {
        // One row per parameter, or a single row for the "no parameters" notice
        // an unresolved plugin id shows.
        const size_t n = HostParamCount(d);
        return h + (n ? (float)n : 1.0f) * kParamRowH + 8;
    }
    if (d.type == EffectType::Eq || d.type == EffectType::Compressor)
        h += kGraphH;
    if (d.type == EffectType::Reverb || d.type == EffectType::Delay)
        h += kSelH;   // type / sync selector row
    h += kKnobH;   // one knob row (all built-ins have <= 5 knobs)
    return h + 8;
}

float EffectsView::PanelTop(size_t i) const {
    // Focus mode draws exactly one panel, so only that index has a position.
    // Anything else is off-screen rather than at 0, which would otherwise stack
    // every hidden panel under the visible one.
    if (fFocus >= 0)
        return (i == (size_t)fFocus) ? kPanelPad : -100000.0f;
    float y = kPanelPad;
    for (size_t k = 0; k < i && k < fChain.size(); k++)
        y += PanelHeight(fChain[k]) + 6;
    return y;
}

float EffectsView::ContentHeight() const {
    if (fFocus >= 0 && fFocus < (int)fChain.size())
        return kPanelPad + PanelHeight(fChain[(size_t)fFocus]) + 6 + kPanelPad;
    float y = kPanelPad;
    for (const EffectDesc& d : fChain) y += PanelHeight(d) + 6;
    y += 26 + 12;   // the single "Add Effect..." button
    return y;
}

void EffectsView::Apply() {
    BMessage m(kMsgApplyFx);
    m.AddInt64("track", (int64)fTrack);
    for (const EffectDesc& d : fChain) {
        m.AddInt32("et", (int32)(int)d.type);
        m.AddInt32("ec", (int32)d.params.size());
        m.AddString("en", d.pluginName.c_str());   // empty for built-ins
        // Insert-slot state travels with the descriptor. This editor doesn't
        // expose it yet (the slot UI is separate), but the snapshot it was
        // opened with may already carry it, and flattening without these would
        // silently reset every insert to un-bypassed / fully wet on the next
        // knob move.
        m.AddInt32("eb", d.bypassed ? 1 : 0);
        m.AddFloat("em", d.mix);
        for (float v : d.params) m.AddFloat("ep", v);
    }
    fApply.SendMessage(&m);
}

// --- drawing --------------------------------------------------------------

void EffectsView::DrawKnob(BRect r, const char* label, float value,
                           float mn, float mx) {
    const float cx = (r.left + r.right) * 0.5f;
    const float cy = r.top + kDialCy;   // pushed down so the dial clears the label
    const float rad = kDialR;
    float t = (mx > mn) ? (value - mn) / (mx - mn) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;

    // Label (centered above, clear of the dial + its tick ring).
    //
    // Built-in labels are written to fit ("Thr dB", "Atk ms"), but plugin
    // parameter names come from the plugin and are routinely wider than a cell
    // -- a 26-param LV2 EQ has "HPF Frequency" next to "LPF Frequency". Left
    // alone they overdraw their neighbours into an unreadable run of text, so
    // clip to the cell and let the ellipsis show there is more.
    SetHighColor(ColText());
    BString lbl(label);
    TruncateString(&lbl, B_TRUNCATE_END, r.Width() - 2.0f);
    DrawString(lbl.String(),
               BPoint(cx - StringWidth(lbl.String()) * 0.5f, r.top + 10));

    // Tick scale around the -135..+135 sweep; the reached ticks are lit.
    for (int i = 0; i <= 10; i++) {
        const double tf = i / 10.0;
        const double a  = (-135.0 + 270.0 * tf) * M_PI / 180.0;
        const float ux = (float)std::sin(a), uy = -(float)std::cos(a);
        SetHighColor(tf <= t + 0.001 ? ColAccent() : ColGrid());
        StrokeLine(BPoint(cx + ux * (rad + 2), cy + uy * (rad + 2)),
                   BPoint(cx + ux * (rad + kDialTick), cy + uy * (rad + kDialTick)));
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
        // Focus mode: one insert only. Skipping here (rather than filtering
        // fChain) keeps every hit and every Apply() addressing the real chain
        // index, so editing this insert cannot disturb its neighbours.
        if (fFocus >= 0 && (int)i != fFocus) continue;
        const EffectDesc& d = fChain[i];
        const float top = PanelTop(i);
        const float ph  = PanelHeight(d);
        BRect panel(kPanelPad, top, w - kPanelPad, top + ph - 6);
        SetHighColor(ColHeader());
        FillRect(panel);
        SetHighColor(ColHeaderHi());
        FillRect(BRect(panel.left, panel.top, panel.right, panel.top + kTitleH));
        SetHighColor(ColText());
        const std::string titleStr = EffectDisplayName(d);
        const char* title = titleStr.c_str();
        DrawString(title, BPoint(panel.left + 8, panel.top + 15));

        // Up / Down / Remove buttons in the title bar.
        auto btn = [&](float rx, const char* lbl, int kind) {
            BRect b(rx, panel.top + 2, rx + kBtnW, panel.top + kTitleH - 2);
            SetHighColor(ColHeaderHi());  FillRect(b);
            SetHighColor(ColGrid());      StrokeRect(b);
            SetHighColor(ColText());      DrawString(lbl, BPoint(b.left + 6, b.bottom - 5));
            fHits.push_back({ (int)i, kind, 0, b, 0, 0 });
        };
        if (fFocus < 0) {          // reordering needs the chain to be visible
            btn(panel.right - 3 * kBtnW - 60, "^", 1);
            btn(panel.right - 2 * kBtnW - 56, "v", 2);
        }
#ifdef DAW_HAVE_LV2
        // "UI" opens the plugin's OWN editor. Only offered when the plugin
        // actually ships one this host can embed, so the button never appears
        // and then does nothing.
        if (d.type == EffectType::Lv2 && Lv2UiWindow::HasNativeUi(d.pluginName)) {
            BRect ub(panel.right - 5 * kBtnW - 62, panel.top + 2,
                     panel.right - 4 * kBtnW - 62 + kBtnW, panel.top + kTitleH - 2);
            SetHighColor(ColHeaderHi()); FillRect(ub);
            SetHighColor(ColGrid());     StrokeRect(ub);
            SetHighColor(ColAccent());
            DrawString("UI", BPoint(ub.left + 4, ub.bottom - 5));
            fHits.push_back({ (int)i, 10, 0, ub, 0, 0 });   // kind 10 = native UI
        }
#endif
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
        } else if (d.type == EffectType::Delay) {
            // Sync selector: "Free" (manual Time knob) or a tempo note-division.
            const bool sync = d.p(3) >= 0.5f;
            const char* nm = sync ? Delay::DivisionName((int)(d.p(4) + 0.5f))
                                  : "Free";
            BRect sel(panel.left + 6, knobTop + 2, panel.right - 6, knobTop + 22);
            SetHighColor(ColHeaderHi()); FillRect(sel);
            SetHighColor(ColGrid());     StrokeRect(sel);
            SetHighColor(ColTextDim());
            DrawString("Sync:", BPoint(sel.left + 8, sel.bottom - 6));
            SetHighColor(ColAccent());
            DrawString(nm, BPoint(sel.left + 52, sel.bottom - 6));
            SetHighColor(ColText());
            DrawString("v", BPoint(sel.right - 14, sel.bottom - 6));
            fHits.push_back({ (int)i, 8, 0, sel, 0, 0 });   // kind 8 = delay sync
            knobTop += kSelH;
        }

        if (UsesParamList(d)) {
            // Generic parameter list: label, slider, value -- one row each, for
            // however many the plugin exposes.
            const std::vector<HostParam> hp = HostParamsFor(d);
            if (hp.empty()) {
                SetHighColor(ColTextDim());
                DrawString("No parameters (plugin not available)",
                           BPoint(panel.left + 10, knobTop + 15));
            }
            float ry = knobTop;
            for (size_t pi = 0; pi < hp.size(); pi++) {
                const HostParam& p = hp[pi];
                const float val = d.p(pi);

                BString lbl(p.name);
                TruncateString(&lbl, B_TRUNCATE_END, kParamLabelW - 6.0f);
                SetHighColor(ColText());
                DrawString(lbl.String(), BPoint(panel.left + 10, ry + 15));

                BRect tr(panel.left + 10 + kParamLabelW, ry + 5,
                         panel.right - 10 - kParamValW, ry + kParamRowH - 9);
                if (tr.Width() > 8) {
                    SetHighColor(ColHeaderHi()); FillRect(tr);
                    SetHighColor(ColGrid());     StrokeRect(tr);
                    float t = (p.mx > p.mn) ? (val - p.mn) / (p.mx - p.mn) : 0.0f;
                    if (t < 0) t = 0; if (t > 1) t = 1;
                    BRect fill(tr.left + 1, tr.top + 1,
                               tr.left + 1 + (tr.Width() - 2) * t, tr.bottom - 1);
                    if (fill.right > fill.left) {
                        SetHighColor(ColAccent()); FillRect(fill);
                    }
                    // kind 9: a horizontal drag across THIS rect, so the rect
                    // is what the drag has to map against (see MouseDown).
                    fHits.push_back({ (int)i, 9, (int)pi, tr, p.mn, p.mx });
                }

                char buf[32];
                std::snprintf(buf, sizeof buf,
                              (p.isInteger || (p.mx - p.mn) >= 100.0f)
                                  ? "%.0f" : "%.2f", val);
                SetHighColor(ColTextDim());
                DrawString(buf, BPoint(panel.right - kParamValW - 2, ry + 15));
                ry += kParamRowH;
            }
        } else {
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
    }

    if (fFocus >= 0) return;   // a single-insert view has nothing to add to

    // ONE button, opening the searchable browser. It used to be a button per
    // available effect, which does not survive a real plugin collection: the
    // column sits below the chain, so a single 26-parameter plugin pushed every
    // add button off the visible area.
    const float ay = PanelTop(fChain.size());
    BRect b(kPanelPad, ay, w - kPanelPad, ay + 22);
    SetHighColor(ColHeaderHi()); FillRect(b);
    SetHighColor(ColGrid());     StrokeRect(b);
    SetHighColor(ColAccent());
    DrawString("Add Effect...", BPoint(b.left + 10, b.bottom - 6));
    fHits.push_back({ 0, 4, 0, b, 0, 0 });   // kind 4 = open the browser
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
        case EffectType::Limiter:    return LimiterDesc();
        case EffectType::Eq:
        default:                     return EqDesc();
    }
}

// Public: build a fresh insert for `type`. Shared by the effects editor and the
// channel strip, because the defaults rule below is a trap worth encoding once.
EffectDesc MakeInsertDesc(EffectType type, const std::string& pluginId) {
    if (!EffectHasPluginName(type)) return MakeDefault(type);

    EffectDesc d;
    d.type       = type;
    d.pluginName = pluginId;
    // Seed the HOST's own port defaults, never zeros. LV2 plugins routinely
    // expose an `Enabled` control defaulting to 1, so a zero-filled descriptor
    // inserts a plugin that renders silence -- indistinguishable, to the user,
    // from a broken host. Leaving params empty would also work (the factory then
    // applies port defaults itself), but seeding them means the editor shows the
    // real values immediately instead of zeros.
    for (const HostParam& p : HostParamsFor(d)) d.params.push_back(p.def);
    return d;
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

    // Double-clicking a parameter slider restores the plugin's own default for
    // that port -- the only way back to it once a value has been dragged, since
    // a generic list has no per-parameter menu.
    int32 clicks = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("clicks", &clicks);
    if (kind == 9 && clicks > 1
        && h.effect >= 0 && h.effect < (int)fChain.size()) {
        EffectDesc& d = fChain[h.effect];
        const std::vector<HostParam> hp = HostParamsFor(d);
        if (h.slot >= 0 && h.slot < (int)hp.size()) {
            EnsureParamSlot(d, h.slot);
            d.params[(size_t)h.slot] = hp[(size_t)h.slot].def;
            fDragEffect = -1;          // the dbl-click is not also a drag
            Apply(); Invalidate();
        }
        return;
    }

    switch (kind) {
        case 0:   // knob: begin a vertical drag
        case 9:   // plugin parameter slider: horizontal drag across its track
        case 5: { // eq handle: 2D drag
            fDragEffect = h.effect; fDragSlot = h.slot; fDragKind = kind;
            fDragMin = h.min; fDragMax = h.max;
            fDragRect = h.rect;
            fDragInteger = false;
            fDragToggled = false;
            fDragScalePoints.clear();
            if (kind == 9 && h.effect >= 0 && h.effect < (int)fChain.size()) {
                const std::vector<HostParam> hp = HostParamsFor(fChain[h.effect]);
                if (h.slot >= 0 && h.slot < (int)hp.size()) {
                    const HostParam& p = hp[(size_t)h.slot];
                    fDragInteger = p.isInteger;
                    fDragToggled = p.isToggled;
                    if (p.scalePoints) fDragScalePoints = *p.scalePoints;
                }
            }
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
        case 8: {   // delay sync / division dropdown
            if (h.effect < 0 || h.effect >= (int)fChain.size()) break;
            EffectDesc& d = fChain[h.effect];
            if (d.params.size() < 5) d.params.resize(5, 0.0f);
            const bool sync = d.p(3) >= 0.5f;
            const int  curDiv = (int)(d.p(4) + 0.5f);
            BPopUpMenu* menu = new BPopUpMenu("sync", false, false);
            BMenuItem* free = new BMenuItem("Free", nullptr);
            if (!sync) free->SetMarked(true);
            menu->AddItem(free);
            for (int a = 0; a < Delay::kDivisionCount; a++) {
                BMenuItem* it = new BMenuItem(Delay::DivisionName(a), nullptr);
                if (sync && a == curDiv) it->SetMarked(true);
                menu->AddItem(it);
            }
            BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
            if (sel) {
                const int idx = menu->IndexOf(sel);
                if (idx == 0) { d.params[3] = 0.0f; }             // Free
                else { d.params[3] = 1.0f; d.params[4] = (float)(idx - 1); }
                Apply(); Invalidate();
            }
            delete menu;
            break;
        }
        case 3:   // remove
            if (h.effect >= 0 && h.effect < (int)fChain.size()) {
                fChain.erase(fChain.begin() + h.effect);
                Apply(); Invalidate(); UpdateScrollRange();
            }
            break;
        case 4:   // "Add Effect..." -> open the browser; it posts the choice back
            OpenBrowser();
            break;
#ifdef DAW_HAVE_LV2
        case 10: {   // open the plugin's own editor
            if (h.effect < 0 || h.effect >= (int)fChain.size()) break;
            const EffectDesc& d = fChain[(size_t)h.effect];
            BRect wr(140, 140, 140 + 960, 140 + 680);
            if (BWindow* w = Window()) {
                BRect f = w->Frame();
                wr.OffsetTo(f.left + 30, f.top + 30);
            }
            // Null return = the plugin has no embeddable UI or its editor
            // refused to instantiate; the generic panel stays as it is.
            Lv2UiWindow::Open(wr, d.pluginName, EffectDisplayName(d), d.params);
            break;
        }
#endif
    }
}

void EffectsView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDragEffect < 0 || fDragEffect >= (int)fChain.size()) return;
    EffectDesc& d = fChain[fDragEffect];
    // Push the changed param straight to the running engine so the effect
    // responds live while dragging (mixing feel); commit is on mouse-up.
    auto live = [&](int slot, float v) {
        BMessage m(kMsgFxLive);
        m.AddInt64("track", (int64)fTrack);
        m.AddInt32("fx", fDragEffect);
        m.AddInt32("slot", slot);
        m.AddFloat("val", v);
        fApply.SendMessage(&m);
    };
    auto setP = [&](int slot, float v) {
        EnsureParamSlot(d, slot);
        d.params[slot] = v;
        live(slot, v);
    };
    if (fDragKind == 0) {   // knob: vertical drag over ~160 px = full range
        const float dv = (fDragStart.y - where.y) / 160.0f * (fDragMax - fDragMin);
        float v = fDragStartVal + dv;
        if (v < fDragMin) v = fDragMin; if (v > fDragMax) v = fDragMax;
        setP(fDragSlot, v);
    } else if (fDragKind == 9) {   // parameter slider: x across the track
        float t = (fDragRect.Width() > 0)
                  ? (where.x - fDragRect.left) / fDragRect.Width() : 0.0f;
        if (t < 0) t = 0; if (t > 1) t = 1;
        float v = fDragMin + t * (fDragMax - fDragMin);
        // Coerce through the SAME function the host uses, so the value shown and
        // the value the plugin receives cannot disagree -- and so a toggle gets
        // an endpoint and an enumeration gets one of its declared points rather
        // than any whole number that happens to lie in range.
        v = ClampLv2Param(v, fDragMin, fDragMax, true, true,
                          fDragInteger, fDragToggled,
                          fDragScalePoints.empty() ? nullptr : &fDragScalePoints);
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

// A wheel gesture has no mouse-up to commit on, so fold its notches into ONE
// undoable edit: every notch previews live and restarts this timer, and the
// commit fires once the wheel goes quiet. (SetFxCommand deliberately does not
// coalesce — it assumes one command per gesture — so committing per notch would
// push a separate undo step for every click of the wheel.)
void EffectsView::ScheduleCommit() {
    BMessage m(kMsgFxCommit);
    // Construct before destroying, so a throwing allocation can't leave fCommit
    // dangling for the destructor to delete a second time.
    BMessageRunner* next = new BMessageRunner(BMessenger(this), &m,
                                              kWheelCommitDelay, 1);
    delete fCommit;                  // restart: only the last notch commits
    fCommit = next;
}

// Wheel over a control edits it instead of scrolling the panel list. Over a knob
// it nudges that knob; over an EQ band handle it nudges that band's Q — the one
// band parameter the 2D graph drag can't reach (drag is freq x gain), so the
// wheel completes the gesture without leaving the curve.
bool EffectsView::WheelAdjust(BPoint where, float dy) {
    Hit h;
    const int kind = HitTest(where, &h);
    int   slot = -1;
    float mn = 0.0f, mx = 1.0f;
    if (kind == 0) {              // knob: its own drawn range
        // A knob's hit cell is deliberately much larger than the dial so that
        // click-drag is forgiving. The wheel must not be: that cell is mostly
        // blank panel, and claiming it would turn an ordinary scroll past the
        // knob row into a silent parameter change. Require the drawn dial.
        const float cx = (h.rect.left + h.rect.right) * 0.5f;
        const float cy = h.rect.top + kDialCy;
        const float ox = where.x - cx, oy = where.y - cy;   // offset from centre
        const float reach = kDialR + kDialTick;
        if (ox * ox + oy * oy > reach * reach) return false;
        slot = h.slot; mn = h.min; mx = h.max;
    } else if (kind == 9) {       // plugin parameter slider
        slot = h.slot; mn = h.min; mx = h.max;
    } else if (kind == 5) {       // EQ band handle -> that band's Q
        slot = h.slot * 3 + 2;
        FxParamRange(EffectType::Eq, slot, &mn, &mx);
    } else {
        return false;             // empty panel space: let the list scroll
    }
    if (h.effect < 0 || h.effect >= (int)fChain.size() || mx <= mn) return false;

    EffectDesc& d = fChain[h.effect];
    EnsureParamSlot(d, slot);
    const float step = (mx - mn)
                     / ((modifiers() & B_SHIFT_KEY) ? kWheelFine : kWheelCoarse);
    // Wheel-up is a negative delta; negate so up raises, matching the knobs'
    // drag-up-to-raise feel.
    float v = d.params[(size_t)slot] - dy * step;
    if (v < mn) v = mn; if (v > mx) v = mx;
    if (v == d.params[(size_t)slot]) return true;   // already at the rail
    d.params[(size_t)slot] = v;

    // Same live path as a knob drag: the running engine hears it immediately.
    BMessage live(kMsgFxLive);
    live.AddInt64("track", (int64)fTrack);
    live.AddInt32("fx", h.effect);
    live.AddInt32("slot", slot);
    live.AddFloat("val", v);
    fApply.SendMessage(&live);

    ScheduleCommit();
    Invalidate();
    return true;
}

// Open the searchable plugin browser. It runs its own looper and posts
// kMsgPluginChosen back to this view; it never touches the model.
void EffectsView::OpenBrowser() {
    BRect wr(160, 160, 620, 560);
    if (BWindow* w = Window()) {
        // Offer it beside the editor rather than at a fixed screen position.
        BRect f = w->Frame();
        wr.OffsetTo(f.right + 12, f.top);
    }
    (new PluginBrowser(wr, fTrack, BMessenger(this), fApply))->Show();
}

void EffectsView::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgPluginChosen) {
        int32 type = 0;
        const char* name = nullptr;
        if (msg->FindInt32("type", &type) != B_OK) return;
        msg->FindString("name", &name);
        if (type < 0 || type > kMaxEffectTypeId) return;   // never trust a message

        const EffectType t = (EffectType)type;
        const std::string id = name ? name : "";
        if (EffectHasPluginName(t) && id.empty()) return;
        fChain.push_back(MakeInsertDesc(t, id));
        Apply(); Invalidate(); UpdateScrollRange();
        return;
    }
    if (msg->what == kMsgFxCommit) {   // wheel went quiet: fold it into one step
        FlushPendingEdit();
        return;
    }
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (msg->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            // The wheel message carries no position; ask for the pointer.
            BPoint pt; uint32 buttons = 0;
            GetMouse(&pt, &buttons, false);
            if (WheelAdjust(pt, dy)) return;
        }
    }
    BView::MessageReceived(msg);
}

// --- window ---------------------------------------------------------------

EffectsWindow::EffectsWindow(BRect frame, std::vector<EffectDesc> chain,
                             TrackId track, BMessenger apply, int focusSlot)
    : BWindow(frame, "Effects", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fTrack(track), fApply(apply) {
    // Name the window after the insert when it opens on one, so several open
    // editors are told apart by their title bars.
    if (focusSlot >= 0 && focusSlot < (int)chain.size())
        SetTitle(EffectDisplayName(chain[(size_t)focusSlot]).c_str());

    BRect b = Bounds();
    BRect vr(b.left, b.top, b.right - B_V_SCROLL_BAR_WIDTH, b.bottom);
    fView = new EffectsView(vr, std::move(chain), track, apply, focusSlot);
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

void EffectsWindow::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardSpaceToTransport(m, fApply)) return;
    BWindow::DispatchMessage(m, h);
}

bool EffectsWindow::QuitRequested() {
    if (fView) fView->FlushPendingEdit();   // don't drop an in-flight wheel edit
    BMessage closed(kMsgFxWinClosed);
    closed.AddInt64("track", (int64)fTrack);
    fApply.SendMessage(&closed);
    return true;
}

} // namespace daw
