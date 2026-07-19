#include "InspectorView.h"

#include "UiMetrics.h"
#include "Widgets.h"
#include "EffectsWindow.h"
#include "PluginBrowser.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "../model/Commands.h"
#include "../midi/MidiPort.h"

#include <PopUpMenu.h>
#include <MenuItem.h>
#include <Window.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace daw {

InspectorView::InspectorView(BRect frame, Project* project, CommandStack* stack)
    : BView(frame, "inspector", B_FOLLOW_LEFT | B_FOLLOW_TOP_BOTTOM, B_WILL_DRAW),
      fProject(project), fStack(stack) {
    SetViewColor(ColHeader());
}

const Track* InspectorView::CurrentTrack() const {
    return fProject ? fProject->FindTrack(fTrack) : nullptr;
}
Track* InspectorView::CurrentTrackMut() const {
    return fProject ? fProject->FindTrack(fTrack) : nullptr;
}

void InspectorView::Refresh() {
    Invalidate();
    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
}

// Insert-slot list metrics. Rows are compact because the strip is shared with
// the fader and meter; kMaxFxRows caps the block so a long chain cannot squeeze
// them out.
static constexpr float  kFxRowH   = 17.0f;
static constexpr size_t kMaxFxRows = 6;
// The bypass dot sits at the right end of a row.
static constexpr float  kFxDotW   = 14.0f;

BRect InspectorView::FxRowRect(int i) const {
    if (i < 0 || i >= fFxRows) return BRect();
    const float top = fFxSlotsR.top + i * kFxRowH;
    return BRect(fFxSlotsR.left, top, fFxSlotsR.right, top + kFxRowH - 2.0f);
}

void InspectorView::Layout() {
    const float w = Bounds().Width();
    const float pad = 10.0f;
    const float bw = w - pad * 2.0f;   // full-width control
    const float hw = (bw - 6.0f) * 0.5f;   // half-width (two side by side)

    float y = 46.0f;
    fMuteR = BRect(pad,           y, pad + 30,      y + 20);
    fSoloR = BRect(pad + 36,      y, pad + 66,      y + 20);
    fArmR  = BRect(pad + 72,      y, pad + 102,     y + 20);
    y += 28;
    fInputR = BRect(pad, y, pad + bw, y + 20); y += 26;
    fMonR   = BRect(pad, y, pad + bw, y + 20); y += 28;
    fOutR   = BRect(pad,           y, pad + hw,     y + 20);
    fSendsR = BRect(pad + hw + 6,  y, pad + bw,     y + 20); y += 26;
    // The instrument slot gets its own full-width row ABOVE the inserts, so the
    // one-instrument-plus-N-inserts model is visible in the strip: a MIDI track
    // has exactly one voice, and any number of inserts after it.
    if (const Track* it = CurrentTrack())
        if (it->type == TrackType::Midi) {
            fInstR = BRect(pad, y, pad + bw, y + 20);
            y += 26;
        } else {
            fInstR = BRect();
        }
    else fInstR = BRect();

    // Insert slots: one row per effect plus a trailing empty one to add into.
    // Capped so a long chain cannot push the fader off the bottom of the strip;
    // the overflow row says how many are hidden and opens the full editor.
    const size_t nfx = CurrentTrack() ? CurrentTrack()->fx.size() : 0;
    fFxRows = (int)(nfx < kMaxFxRows ? nfx + 1 : kMaxFxRows);
    fFxSlotsR = BRect(pad, y, pad + bw, y + fFxRows * kFxRowH);
    y += fFxRows * kFxRowH + 6;
    fFxR = BRect();             // the old single "FX n" button is gone

    fAutoR  = BRect(pad, y, pad + bw, y + 20);
    y += 20 + 20;               // button + a labelled gap ("Pan") before the knob
    // Pan knob (centered); its "Pan" label sits in the gap above it.
    const float knob = 46.0f;
    fPanR   = BRect(w * 0.5f - knob * 0.5f, y, w * 0.5f + knob * 0.5f, y + knob);
    y += knob + 18;
    // Fader (left) + VU meter (right), side by side, centered as a unit.
    const float bottom = Bounds().bottom - 26.0f;
    fFaderR = BRect(w * 0.5f - 42.0f, y, w * 0.5f - 4.0f, bottom);
    fMeterR = BRect(w * 0.5f + 6.0f,  y, w * 0.5f + 24.0f, bottom);
}

void InspectorView::Draw(BRect) {
    Layout();
    SetHighColor(ColHeader());
    FillRect(Bounds());
    SetHighColor(ColGrid());
    StrokeLine(BPoint(Bounds().right, Bounds().top),
               BPoint(Bounds().right, Bounds().bottom));

    const Track* t = CurrentTrack();
    if (!t) {
        SetHighColor(ColTextDim());
        DrawString("No track selected", BPoint(12, 40));
        return;
    }
    const bool midi = t->type == TrackType::Midi;

    // Header: color stripe + name + type.
    SetHighColor(TrackColor(t->colorIndex));
    FillRect(BRect(0, 0, Bounds().right, 4));
    SetHighColor(ColText());
    BFont bold(be_bold_font); bold.SetSize(13); SetFont(&bold);
    DrawString(t->name.c_str(), BPoint(10, 24));
    SetFont(be_plain_font);
    SetHighColor(ColTextDim());
    DrawString(midi ? "MIDI" : t->type == TrackType::Bus ? "Bus" : "Audio",
               BPoint(10, 40));

    const rgb_color accent = midi ? ColMidiAccent() : ColAudioAccent();
    DrawButton(this, fMuteR, "M", t->muted,  ColMute());
    DrawButton(this, fSoloR, "S", t->soloed, ColSolo());
    DrawButton(this, fArmR,  "R", t->armed,  ColRec());

    // Input source.
    char ib[64];
    if (t->input.kind == InputSource::kMidi) {
        std::string n = t->input.name;
        const std::string pfx = "/dev/midi/";
        if (n.compare(0, pfx.size(), pfx) == 0) n = n.substr(pfx.size());
        std::snprintf(ib, sizeof(ib), "In: %s", n.empty() ? "set" : n.c_str());
    } else if (t->input.kind == InputSource::kAudioDefault) {
        std::snprintf(ib, sizeof(ib), "In: Default");
    } else {
        std::snprintf(ib, sizeof(ib), "In: \xE2\x80\x94");
    }
    DrawButton(this, fInputR, ib, t->input.kind != InputSource::kNone,
               Rgb(52, 104, 74));

    DrawButton(this, fMonR, "Input Monitor", t->inputMonitor, ColMon());

    // Output routing + sends.
    DrawButton(this, fOutR,
               t->output == kInvalidTrackId ? "Out: Mst" : "Out: Bus",
               t->output != kInvalidTrackId, accent);
    char sb[16];
    std::snprintf(sb, sizeof(sb), "Sends %zu", t->sends.size());
    DrawButton(this, fSendsR, sb, !t->sends.empty(), accent);

    // Instrument slot (MIDI only), visually distinct from the inserts below it.
    if (midi && fInstR.IsValid())
        DrawButton(this, fInstR, "Instrument", true, ColMidiAccent());

    // --- Insert slots ------------------------------------------------------
    const size_t nfx = t->fx.size();
    for (int i = 0; i < fFxRows; i++) {
        const BRect r = FxRowRect(i);
        if (!r.IsValid()) continue;

        // The last row is the overflow notice when the chain is longer than the
        // strip can show, and the "add" row otherwise.
        const bool overflow = nfx >= kMaxFxRows && i == (int)kMaxFxRows - 1;
        const bool empty    = !overflow && (size_t)i >= nfx;

        SetHighColor(ColHeaderHi());
        FillRect(r);
        SetHighColor(ColGrid());
        StrokeRect(r);

        if (overflow) {
            char ob[32];
            std::snprintf(ob, sizeof(ob), "+%zu more...",
                          nfx - (kMaxFxRows - 1));
            SetHighColor(ColTextDim());
            DrawString(ob, BPoint(r.left + 6, r.bottom - 5));
            continue;
        }
        if (empty) {
            SetHighColor(ColTextDim());
            DrawString("+ Add Effect", BPoint(r.left + 6, r.bottom - 5));
            continue;
        }

        const EffectDesc& d = t->fx[(size_t)i];
        // A bypassed insert keeps its slot but reads as inactive.
        SetHighColor(d.bypassed ? ColTextDim() : ColText());
        BString nm(EffectDisplayName(d).c_str());
        TruncateString(&nm, B_TRUNCATE_END, r.Width() - kFxDotW - 12.0f);
        DrawString(nm.String(), BPoint(r.left + 6, r.bottom - 5));

        // Bypass dot: filled in the track accent when the insert is live,
        // hollow when bypassed.
        BRect dot(r.right - kFxDotW - 2, r.top + 3,
                  r.right - 4, r.top + 3 + (kFxDotW - 6));
        if (d.bypassed) {
            SetHighColor(ColTextDim());
            StrokeEllipse(dot);
        } else {
            SetHighColor(accent);
            FillEllipse(dot);
        }
    }

    // Drop indicator: the row the dragged insert will OCCUPY, framed in the
    // track accent.
    //
    // Deliberately a row highlight and not an insertion line between rows. The
    // drop target is computed as "the row under the pointer" and clamped to the
    // real inserts, so it names a slot, not a gap -- drag an insert onto row 3
    // and it takes row 3, pushing the previous occupant aside. A line drawn
    // above the row would promise "insert BEFORE this one", which is a different
    // result whenever the drag moves downward, and the code would be right while
    // the drawing lied.
    if (fDrag == Drag::FxSlot && fDragFxTo >= 0 && fDragFxTo < fFxRows) {
        const BRect r = FxRowRect(fDragFxTo);
        if (r.IsValid()) {
            SetHighColor(accent);
            StrokeRect(r);
            StrokeRect(r.InsetByCopy(1.0f, 1.0f));   // 2 px: reads as a target
        }
    }

    DrawButton(this, fAutoR, "Automation", false);

    // Pan knob (value ring in the track accent). Label centered above it.
    SetHighColor(ColTextDim());
    const float pw = StringWidth("Pan");
    DrawString("Pan", BPoint((fPanR.left + fPanR.right) * 0.5f - pw * 0.5f,
                             fPanR.top - 7));
    DrawKnob(this, fPanR, t->pan, accent);

    // Channel fader + VU meter (peak-fed) side by side.
    DrawFader(this, fFaderR, GainToFrac(t->gain), kUnityFrac);
    DrawVUMeter(this, fMeterR, std::max(fPeakL, fPeakR));

    char db[16];
    const float d = GainToDb(t->gain);
    if (d <= -80.0f) std::snprintf(db, sizeof(db), "-inf");
    else             std::snprintf(db, sizeof(db), "%+.1f dB", d);
    SetHighColor(ColText());
    const float tw = StringWidth(db);
    DrawString(db, BPoint(fFaderR.left, Bounds().bottom - 8));
}

void InspectorView::MouseDown(BPoint where) {
    Track* t = CurrentTrackMut();
    if (!t) return;
    const TrackId id = t->id;

    if (fMuteR.Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackMuteCommand>(id, !t->muted), *fProject);
        Refresh(); return;
    }
    if (fSoloR.Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackSoloCommand>(id, !t->soloed), *fProject);
        Refresh(); return;
    }
    if (fArmR.Contains(where)) {
        t->armed = !t->armed;               // transient state, not undoable
        Refresh();
        if (BWindow* w = Window()) w->PostMessage(kMsgMonitorRefresh);
        return;
    }
    if (fMonR.Contains(where)) {
        t->inputMonitor = !t->inputMonitor;   // per-track; hear input w/o arming
        Refresh();
        if (BWindow* w = Window()) w->PostMessage(kMsgMonitorRefresh);
        return;
    }
    if (fInputR.Contains(where)) {
        BPopUpMenu* menu = new BPopUpMenu("input", false, false);
        BMenuItem* none = new BMenuItem("None", NULL);
        if (t->input.kind == InputSource::kNone) none->SetMarked(true);
        menu->AddItem(none);
        std::vector<InputSource> choices;
        if (t->type == TrackType::Midi) {
            for (const MidiEndpointInfo& e : EnumerateMidiEndpoints()) {
                if (!e.isProducer || e.name.find("HaikuDAW") != std::string::npos)
                    continue;
                BMenuItem* it = new BMenuItem(e.name.c_str(), NULL);
                if (t->input.kind == InputSource::kMidi && t->input.name == e.name)
                    it->SetMarked(true);
                menu->AddItem(it);
                choices.push_back(InputSource{ InputSource::kMidi, e.name, 0 });
            }
        } else if (t->type == TrackType::Audio) {
            BMenuItem* def = new BMenuItem("Default Input", NULL);
            if (t->input.kind == InputSource::kAudioDefault) def->SetMarked(true);
            menu->AddItem(def);
            choices.push_back(InputSource{ InputSource::kAudioDefault, "", 0 });
        }
        BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
        const int32 pick = sel ? menu->IndexOf(sel) : -1;
        delete menu;
        if (pick == 0)
            fStack->Execute(std::make_unique<SetTrackInputCommand>(id, InputSource{}), *fProject);
        else if (pick > 0 && (size_t)(pick - 1) < choices.size())
            fStack->Execute(std::make_unique<SetTrackInputCommand>(
                id, choices[(size_t)(pick - 1)]), *fProject);
        Refresh();
        if (BWindow* w = Window()) w->PostMessage(kMsgMonitorRefresh);
        return;
    }
    if (fOutR.Contains(where)) {
        BPopUpMenu* menu = new BPopUpMenu("out", false, false);
        menu->AddItem(new BMenuItem("Master", NULL));
        std::vector<TrackId> targets;
        for (const Track& bt : fProject->Tracks()) {
            if (bt.type != TrackType::Bus || bt.id == id) continue;
            menu->AddItem(new BMenuItem(bt.name.c_str(), NULL));
            targets.push_back(bt.id);
        }
        BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
        const int32 pick = sel ? menu->IndexOf(sel) : -1;
        delete menu;
        if (pick == 0)
            fStack->Execute(std::make_unique<SetTrackOutputCommand>(id, kInvalidTrackId), *fProject);
        else if (pick > 0 && (size_t)(pick - 1) < targets.size())
            fStack->Execute(std::make_unique<SetTrackOutputCommand>(id, targets[(size_t)(pick - 1)]), *fProject);
        Refresh();
        return;
    }
    if (fSendsR.Contains(where)) {
        std::vector<std::pair<TrackId, std::string>> buses;
        for (const Track& bt : fProject->Tracks())
            if (bt.type == TrackType::Bus && bt.id != id)
                buses.push_back({bt.id, bt.name});
        BPoint p = ConvertToScreen(where);
        BRect wr(p.x, p.y, p.x + 340, p.y + 320);
        (new SendsWindow(wr, t->sends, buses, id, BMessenger(Window())))->Show();
        return;
    }
    // --- Insert slots ------------------------------------------------------
    if (fFxSlotsR.Contains(where)) {
        const size_t nfx = t->fx.size();
        for (int i = 0; i < fFxRows; i++) {
            const BRect r = FxRowRect(i);
            if (!r.IsValid() || !r.Contains(where)) continue;

            const bool overflow = nfx >= kMaxFxRows && i == (int)kMaxFxRows - 1;
            const bool empty    = !overflow && (size_t)i >= nfx;

            if (overflow) {              // the rest of the chain: open the editor
                BPoint p = ConvertToScreen(where);
                BRect wr(p.x, p.y, p.x + 480, p.y + 620);
                (new EffectsWindow(wr, t->fx, id, BMessenger(Window())))->Show();
                return;
            }
            if (empty) {                 // add into the free slot
                BPoint p = ConvertToScreen(where);
                BRect wr(p.x, p.y, p.x + 460, p.y + 400);
                (new PluginBrowser(wr, id, BMessenger(this),
                                   BMessenger(Window())))->Show();
                return;
            }

            // The bypass dot is a discrete toggle, so it gets its own command
            // rather than a whole-chain replace: the Edit menu then reads
            // "Bypass Effect" instead of "Edit Effects".
            if (where.x >= r.right - kFxDotW - 4) {
                fStack->Execute(std::make_unique<SetFxBypassCommand>(
                                    id, i, !t->fx[(size_t)i].bypassed),
                                *fProject);
                Refresh();
                return;
            }

            // Anywhere else on the row: begin a possible reorder drag. A click
            // that never moves opens the editor instead (see MouseUp).
            fDrag = Drag::FxSlot;
            fDragFxFrom = i;
            fDragFxTo   = i;
            fDragFxTrack = id;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
        return;
    }
    if (t->type == TrackType::Midi && fInstR.Contains(where)) {
        BPoint p = ConvertToScreen(where);
        BRect wr(p.x, p.y, p.x + 280, p.y + 190);
        (new InstrumentWindow(wr, t->instrument, id, BMessenger(Window())))->Show();
        return;
    }
    if (fAutoR.Contains(where)) {   // cycle the timeline's automation lane mode
        if (BWindow* w = Window()) w->PostMessage(kMsgCycleAuto);
        return;
    }

    // Fader / pan drags (preview by writing the model, commit on mouse-up).
    if (fFaderR.Contains(where)) {
        fDrag = Drag::Fader; fDragOrig = t->gain;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        MouseMoved(where, 0, nullptr);
        return;
    }
    if (fPanR.Contains(where)) {
        fDrag = Drag::Pan; fDragOrig = t->pan; fDragStartY = where.y;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;   // no jump: dragging changes it relative to here
    }
}

// The plugin browser's choice, when it was opened from an empty insert slot.
// The inspector runs on the main window's thread and owns the command stack, so
// it applies the edit directly -- unlike the aux windows, which must post.
void InspectorView::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgPluginChosen) {
        Track* t = CurrentTrackMut();
        int32 type = 0;
        const char* name = nullptr;
        if (!t || msg->FindInt32("type", &type) != B_OK) return;
        msg->FindString("name", &name);
        if (type < 0 || type > kMaxEffectTypeId) return;   // never trust a message

        std::vector<EffectDesc> chain = t->fx;
        chain.push_back(MakeInsertDesc((EffectType)type, name ? name : ""));
        fStack->Execute(std::make_unique<SetFxCommand>(t->id, false,
                                                       std::move(chain)),
                        *fProject);
        Refresh();
        return;
    }
    BView::MessageReceived(msg);
}

void InspectorView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None) return;
    Track* t = CurrentTrackMut();
    if (!t) return;
    if (fDrag == Drag::FxSlot) {
        // Which insert position the pointer is over. Clamped to the real
        // inserts, so a drag onto the empty/overflow row lands at the end.
        const int nfx = (int)t->fx.size();
        int to = (int)((where.y - fFxSlotsR.top) / kFxRowH);
        if (to < 0) to = 0;
        if (to > nfx - 1) to = nfx - 1;
        if (to != fDragFxTo) { fDragFxTo = to; Invalidate(); }
        return;
    }
    if (fDrag == Drag::Fader) {
        if (fFaderR.Height() <= 0) return;   // avoid div-by-zero -> NaN gain
        float frac = (fFaderR.bottom - where.y) / fFaderR.Height();
        t->gain = FracToGain(frac);
    } else if (fDrag == Drag::Pan) {
        // Vertical drag (up = right), relative to the grab point.
        float pan = fDragOrig + (fDragStartY - where.y) / 100.0f;
        if (pan < -1) pan = -1; if (pan > 1) pan = 1;
        t->pan = pan;
    }
    Refresh();
}

void InspectorView::MouseUp(BPoint) {
    if (fDrag == Drag::None) return;
    Track* t = CurrentTrackMut();
    const Drag mode = fDrag;
    fDrag = Drag::None;
    if (!t) return;
    if (mode == Drag::FxSlot) {
        const int from = fDragFxFrom, to = fDragFxTo;
        const TrackId dragTrack = fDragFxTrack;
        fDragFxFrom = fDragFxTo = -1;
        fDragFxTrack = kInvalidTrackId;
        // Row indices address the chain they were taken from. If the selection
        // moved on since, they mean nothing here.
        if (dragTrack != t->id) { Refresh(); return; }
        const int nfx = (int)t->fx.size();
        if (from < 0 || from >= nfx) { Refresh(); return; }
        if (to < 0 || to >= nfx || to == from) {
            // Never moved: this was a click on the row, which opens the editor.
            BRect wr(120, 120, 600, 740);
            (new EffectsWindow(wr, t->fx, t->id, BMessenger(Window())))->Show();
            Refresh();
            return;
        }
        // Reorder is just a permutation of the descriptor vector, so it goes
        // through the ordinary chain-replace command and is one undo step.
        std::vector<EffectDesc> chain = t->fx;
        EffectDesc moved = chain[(size_t)from];
        chain.erase(chain.begin() + from);
        chain.insert(chain.begin() + to, moved);
        fStack->Execute(std::make_unique<SetFxCommand>(t->id, false,
                                                       std::move(chain)),
                        *fProject);
        Refresh();
        return;
    }
    // Restore the pre-drag value, then push ONE command (clean single undo).
    // Only when the value actually changed, so a bare click isn't a no-op undo.
    if (mode == Drag::Fader) {
        const float g = t->gain; t->gain = fDragOrig;
        if (g != fDragOrig)
            fStack->Execute(std::make_unique<SetTrackGainCommand>(t->id, g), *fProject);
    } else if (mode == Drag::Pan) {
        const float p = t->pan; t->pan = fDragOrig;
        if (p != fDragOrig)
            fStack->Execute(std::make_unique<SetTrackPanCommand>(t->id, p), *fProject);
    }
    Refresh();
}

} // namespace daw
