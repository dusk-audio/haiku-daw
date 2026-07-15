#include "InspectorView.h"

#include "UiMetrics.h"
#include "Widgets.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "../model/Commands.h"
#include "../midi/MidiPort.h"

#include <PopUpMenu.h>
#include <MenuItem.h>
#include <Window.h>

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
    fFxR    = BRect(pad,           y, pad + hw,     y + 20);
    fInstR  = BRect(pad + hw + 6,  y, pad + bw,     y + 20); y += 26;
    fAutoR  = BRect(pad, y, pad + bw, y + 20); y += 30;
    // Pan knob (centered) then a tall vertical fader below it.
    const float knob = 44.0f;
    fPanR   = BRect(w * 0.5f - knob * 0.5f, y, w * 0.5f + knob * 0.5f, y + knob);
    y += knob + 16;
    const float faderW = 26.0f;
    fFaderR = BRect(w * 0.5f - faderW * 0.5f, y,
                    w * 0.5f + faderW * 0.5f, Bounds().bottom - 26);
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

    DrawButton(this, fMuteR, "M", t->muted,  ColPlayhead());
    DrawButton(this, fSoloR, "S", t->soloed, Rgb(210, 190, 70));
    DrawButton(this, fArmR,  "R", t->armed,  Rgb(220, 60, 60));

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

    DrawButton(this, fMonR, "Input Monitor", false);

    // Output routing + sends.
    DrawButton(this, fOutR,
               t->output == kInvalidTrackId ? "Out: Mst" : "Out: Bus",
               t->output != kInvalidTrackId, Rgb(70, 90, 130));
    char sb[16];
    std::snprintf(sb, sizeof(sb), "Sends %zu", t->sends.size());
    DrawButton(this, fSendsR, sb, !t->sends.empty(), Rgb(70, 90, 130));

    // Effects + instrument (MIDI).
    char fb[16];
    std::snprintf(fb, sizeof(fb), "FX %zu", t->fx.size());
    DrawButton(this, fFxR, fb, !t->fx.empty(), Rgb(80, 170, 110));
    if (midi) DrawButton(this, fInstR, "Instrument", true, Rgb(70, 90, 130));

    DrawButton(this, fAutoR, "Automation", false);

    // Pan knob.
    SetHighColor(ColTextDim());
    DrawString("Pan", BPoint(fPanR.left - 2, fPanR.top - 4));
    DrawPanKnob(this, fPanR, t->pan);

    // Vertical fader + dB readout.
    DrawVFader(this, fFaderR, GainToFrac(t->gain), kUnityFrac);
    char db[16];
    const float d = GainToDb(t->gain);
    if (d <= -80.0f) std::snprintf(db, sizeof(db), "-inf");
    else             std::snprintf(db, sizeof(db), "%+.1f dB", d);
    SetHighColor(ColText());
    const float tw = StringWidth(db);
    DrawString(db, BPoint(Bounds().Width() * 0.5f - tw * 0.5f,
                          Bounds().bottom - 8));
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
        if (BWindow* w = Window()) w->PostMessage(kMsgInputMon);
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
    if (fFxR.Contains(where)) {
        BPoint p = ConvertToScreen(where);
        BRect wr(p.x, p.y, p.x + 480, p.y + 620);
        (new EffectsWindow(wr, t->fx, id, BMessenger(Window())))->Show();
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
        fDrag = Drag::Pan; fDragOrig = t->pan;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        MouseMoved(where, 0, nullptr);
        return;
    }
}

void InspectorView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fDrag == Drag::None) return;
    Track* t = CurrentTrackMut();
    if (!t) return;
    if (fDrag == Drag::Fader) {
        if (fFaderR.Height() <= 0) return;   // avoid div-by-zero -> NaN gain
        float frac = (fFaderR.bottom - where.y) / fFaderR.Height();
        t->gain = FracToGain(frac);
    } else if (fDrag == Drag::Pan) {
        // Horizontal drag across the knob spans full L..R.
        float pan = (where.x - (fPanR.left + fPanR.right) * 0.5f) / 60.0f;
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
    // Restore the pre-drag value, then push ONE command (clean single undo).
    if (mode == Drag::Fader) {
        const float g = t->gain; t->gain = fDragOrig;
        fStack->Execute(std::make_unique<SetTrackGainCommand>(t->id, g), *fProject);
    } else if (mode == Drag::Pan) {
        const float p = t->pan; t->pan = fDragOrig;
        fStack->Execute(std::make_unique<SetTrackPanCommand>(t->id, p), *fProject);
    }
    Refresh();
}

} // namespace daw
