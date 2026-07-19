#include "InstrumentWindow.h"

#include "UiMetrics.h"
#include "../synth/SampleBank.h"

#include <Application.h>
#include <Cursor.h>
#include <Button.h>
#include <Entry.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Slider.h>
#include <StringView.h>

#include <cstdio>
#include <cstring>
#include <strings.h>   // strcasecmp
#include <utility>

namespace daw {

enum {
    MSG_WAVE   = 'iwav',   // waveform chosen (field "wave")
    MSG_ADSR   = 'iadr',   // an ADSR slider moved (fields "slot","min","max")
    MSG_MODE   = 'imod',   // voice kind chosen (field "type")
    MSG_PICK   = 'ipck',   // "Load..." pressed
    MSG_PICKED = 'ipkd',   // the file panel returned refs
    MSG_PRESET = 'ipst',   // SF2 preset chosen (field "preset")
    MSG_REBUILD = 'irbd',  // deferred Build(), see RebuildLater()
};

namespace {

// Only let the user pick things this build can actually play. Directories stay
// visible so they can navigate into a library.
class SoundfontFilter : public BRefFilter {
public:
    bool Filter(const entry_ref* ref, BNode* node, struct stat_beos* /*st*/,
                const char* /*mime*/) override {
        if (node && node->IsDirectory()) return true;
        if (!ref || !ref->name) return false;
        const char* dot = strrchr(ref->name, '.');
        if (!dot) return false;
        return strcasecmp(dot, ".sfz") == 0 || strcasecmp(dot, ".sf2") == 0;
    }
};

const char* TypeName(InstrumentType t) {
    switch (t) {
        case InstrumentType::Sfz: return "SFZ";
        case InstrumentType::Sf2: return "SF2";
        case InstrumentType::Synth:
        default: return "Synth";
    }
}

// Leaf name of a path, for the file label.
std::string LeafOf(const std::string& p) {
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

} // namespace

InstrumentWindow::InstrumentWindow(BRect frame, InstrumentDesc inst,
                                   TrackId track, BMessenger apply)
    : BWindow(frame, "Instrument", B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS),
      fDesc(std::move(inst)), fTrack(track), fApply(apply) {
    fRoot = new BView(Bounds(), "root", B_FOLLOW_ALL_SIDES, B_WILL_DRAW);
    fRoot->SetViewColor(ColHeader());
    AddChild(fRoot);

    // Reopening the editor on a track that already has a soundfont should show
    // its state, not a blank row — but LOOK UP only. Decoding here would block
    // this window's looper before it is even on screen, so a big kit would make
    // the editor appear frozen the moment it opened. The project load
    // (MainWindow::PrimeSoundfonts) has normally cached it already.
    if (fDesc.UsesSoundfont() && !fDesc.path.empty()) {
        if (fDesc.type == InstrumentType::Sf2) RefreshPresets();
        LoadSoundfontNow(false);
    }
    Build();
}

InstrumentWindow::~InstrumentWindow() {
    delete fPanel;
    delete fFilter;   // the panel never owned it
}

// Rebuild the view tree on a LATER pass of the message loop, never inline in
// the handler for a menu selection.
//
// Build() deletes every child, including the BMenuField whose popup sent the
// message. A BMenuField tracks its menu on its own thread: the item invoke only
// QUEUES the message here, and that tracking code is still unwinding and still
// wants the window lock when we get it. Deleting the menu underneath it wedges
// the window — it stops repainting and stops answering the close box. Posting
// to ourselves lets the menu finish first.
void InstrumentWindow::RebuildLater() {
    PostMessage(MSG_REBUILD);
}

void InstrumentWindow::Apply() {
    BMessage m(kMsgApplyInstrument);
    m.AddInt64("track", (int64)fTrack);
    m.AddInt32("type", (int32)fDesc.type);
    m.AddInt32("wave", fDesc.synth.waveform);
    m.AddFloat("a", fDesc.synth.attack);
    m.AddFloat("d", fDesc.synth.decay);
    m.AddFloat("s", fDesc.synth.sustain);
    m.AddFloat("r", fDesc.synth.release);
    m.AddString("path", fDesc.path.c_str());
    m.AddInt32("preset", fDesc.sf2Preset);
    fApply.SendMessage(&m);
}

void InstrumentWindow::RefreshPresets() {
    fPresets.clear();
    if (fDesc.type != InstrumentType::Sf2 || fDesc.path.empty()) return;
    std::string err;
    fPresets = ListSf2Presets(fDesc.path, &err);
    if (fDesc.sf2Preset < 0 || fDesc.sf2Preset >= (int)fPresets.size())
        fDesc.sf2Preset = 0;
}

void InstrumentWindow::LoadSoundfontNow(bool allowDecode) {
    fLoadOk = false;
    fPartial = false;
    fStatus.clear();
    if (!fDesc.UsesSoundfont() || fDesc.path.empty()) return;

    SoundfontCache& cache = SoundfontCache::Instance();
    LoadedInstrumentPtr inst = cache.Get(fDesc.path, fDesc.sf2Preset);

    if (!inst) {
        if (!allowDecode) {
            // Opening the editor must never block: report what the cache
            // already holds and let an explicit action pay for a decode.
            fStatus = "not loaded";
            return;
        }
        // Decoding a big kit takes seconds and blocks this window's looper —
        // show a real busy cursor so it doesn't just look wedged. (The audio
        // thread is unaffected; only this editor stalls.)
        BCursor busy(B_CURSOR_ID_PROGRESS);
        if (be_app) be_app->SetCursor(&busy);
        std::string err;
        inst = cache.Load(fDesc.path, fDesc.sf2Preset, &err);
        if (be_app) be_app->SetCursor(B_CURSOR_SYSTEM_DEFAULT);
        if (!inst) {
            fStatus = err.empty() ? "could not load" : err;
            return;
        }
    }
    fLoadOk = true;
    // A partial load still plays, but the kit is missing samples — say so
    // rather than reporting the truncated counts as if they were the whole
    // instrument.
    fPartial = !inst->warning.empty();

    size_t bytes = 0;
    for (const SampleData& s : inst->samples)
        bytes += s.data.size() * sizeof(float);
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu regions, %zu samples, %.1f MB",
                  inst->regions.size(), inst->samples.size(), bytes / 1048576.0);
    fStatus = buf;
    if (fPartial) fStatus += "  \xE2\x80\x94 INCOMPLETE: " + inst->warning;
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

    // Voice kind.
    {
        BPopUpMenu* menu = new BPopUpMenu("mode");
        const InstrumentType kinds[3] = { InstrumentType::Synth,
                                          InstrumentType::Sfz,
                                          InstrumentType::Sf2 };
        for (int i = 0; i < 3; i++) {
            BMessage* mm = new BMessage(MSG_MODE);
            mm->AddInt32("type", (int32)kinds[i]);
            BMenuItem* it = new BMenuItem(TypeName(kinds[i]), mm);
            it->SetMarked(kinds[i] == fDesc.type);
            it->SetTarget(this);
            menu->AddItem(it);
        }
        fRoot->AddChild(new BMenuField(BRect(8, y, w - 8, y + 20), "md",
                                       "Voice:", menu));
        y += 30;
    }

    if (!fDesc.UsesSoundfont()) {
        // --- built-in synth: waveform + ADSR -----------------------------
        BPopUpMenu* menu = new BPopUpMenu("wave");
        const char* names[4] = { "Sine", "Saw", "Square", "Triangle" };
        for (int i = 0; i < 4; i++) {
            BMessage* mm = new BMessage(MSG_WAVE);
            mm->AddInt32("wave", i);
            BMenuItem* it = new BMenuItem(names[i], mm);
            it->SetMarked(i == fDesc.synth.waveform);
            it->SetTarget(this);
            menu->AddItem(it);
        }
        fRoot->AddChild(new BMenuField(BRect(8, y, w - 8, y + 20),
                                       "wf", "Waveform:", menu));
        y += 30;

        auto add = [&](const char* lbl, int slot, float mn, float mx, float v) {
            fRoot->AddChild(AdsrRow(BRect(8, y, w - 8, y + 26), lbl, slot,
                                    mn, mx, v, this));
            y += 32;
        };
        add("Attack (s)",  0, 0.0f, 1.0f, fDesc.synth.attack);
        add("Decay (s)",   1, 0.0f, 1.0f, fDesc.synth.decay);
        add("Sustain",     2, 0.0f, 1.0f, fDesc.synth.sustain);
        add("Release (s)", 3, 0.0f, 2.0f, fDesc.synth.release);
    } else {
        // --- soundfont: file, preset, status ------------------------------
        BButton* pick = new BButton(BRect(8, y, 88, y + 24), "pick", "Load...",
                                    new BMessage(MSG_PICK));
        pick->SetTarget(this);
        fRoot->AddChild(pick);

        BStringView* file = new BStringView(BRect(96, y + 4, w - 8, y + 24),
            "file", fDesc.path.empty() ? "(no file)"
                                       : LeafOf(fDesc.path).c_str());
        file->SetHighColor(ColText());
        fRoot->AddChild(file);
        y += 32;

        // The preset popup only earns its space when there is a choice, the
        // same rule DuskStudio's editor uses.
        if (fDesc.type == InstrumentType::Sf2 && fPresets.size() > 1) {
            BPopUpMenu* menu = new BPopUpMenu("preset");
            for (const Sf2PresetInfo& p : fPresets) {
                BMessage* mm = new BMessage(MSG_PRESET);
                mm->AddInt32("preset", p.index);
                char label[96];
                std::snprintf(label, sizeof label, "%03d:%03d  %s",
                              p.bank, p.program, p.name.c_str());
                BMenuItem* it = new BMenuItem(label, mm);
                it->SetMarked(p.index == fDesc.sf2Preset);
                it->SetTarget(this);
                menu->AddItem(it);
            }
            fRoot->AddChild(new BMenuField(BRect(8, y, w - 8, y + 20), "ps",
                                           "Preset:", menu));
            y += 30;
        }

        BStringView* st = new BStringView(BRect(8, y, w - 8, y + 20), "st",
                                          fStatus.c_str());
        // A failed load is the one thing here the user must not miss.
        st->SetHighColor(!fLoadOk ? ColRec()
                                  : (fPartial ? ColMon() : ColTextDim()));
        fRoot->AddChild(st);
        y += 26;

        if (!fDesc.path.empty() && !fLoadOk) {
            BStringView* hint = new BStringView(BRect(8, y, w - 8, y + 20),
                "hint", "Track falls back to the synth voice.");
            hint->SetHighColor(ColTextDim());
            fRoot->AddChild(hint);
            y += 26;
        }
    }

    // Fit the window to whichever pane is showing.
    ResizeTo(w, y + 8);
}

void InstrumentWindow::DispatchMessage(BMessage* m, BHandler* h) {
    if (ForwardSpaceToTransport(m, fApply)) return;
    BWindow::DispatchMessage(m, h);
}

void InstrumentWindow::MessageReceived(BMessage* msg) {
    switch (msg->what) {
        case MSG_MODE: {
            int32 t = 0;
            msg->FindInt32("type", &t);
            if (t < 0 || t > kMaxInstrumentTypeId) t = 0;
            const InstrumentType nt = (InstrumentType)t;
            if (nt == fDesc.type) break;
            fDesc.type = nt;
            // Switching between SFZ and SF2 keeps the old path, which is now
            // the wrong kind of file; drop it rather than show a stale name.
            if (fDesc.UsesSoundfont()) {
                const bool wantSf2 = (nt == InstrumentType::Sf2);
                const size_t dot = fDesc.path.find_last_of('.');
                const bool haveSf2 = dot != std::string::npos
                                  && strcasecmp(fDesc.path.c_str() + dot, ".sf2") == 0;
                if (wantSf2 != haveSf2) { fDesc.path.clear(); fDesc.sf2Preset = 0; }
                RefreshPresets();
                LoadSoundfontNow();
            }
            Apply();
            RebuildLater();
            break;
        }
        case MSG_PICK: {
            if (!fPanel) {
                // BFilePanel COPIES the messenger, so a local is enough (the
                // rest of the app's panels are built the same way).
                BMessenger to(this);
                fPanel = new BFilePanel(B_OPEN_PANEL, &to, nullptr,
                                        B_FILE_NODE, false,
                                        new BMessage(MSG_PICKED));
                fFilter = new SoundfontFilter();
                fPanel->SetRefFilter(fFilter);
            }
            fPanel->Show();
            break;
        }
        case MSG_PICKED: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) != B_OK) break;
            BPath p(&ref);
            if (p.InitCheck() != B_OK) break;
            fDesc.path      = p.Path();
            fDesc.sf2Preset = 0;
            // Follow the file's extension: picking a .sf2 while the popup says
            // SFZ should just work.
            const size_t dot = fDesc.path.find_last_of('.');
            if (dot != std::string::npos
                && strcasecmp(fDesc.path.c_str() + dot, ".sf2") == 0)
                fDesc.type = InstrumentType::Sf2;
            else
                fDesc.type = InstrumentType::Sfz;
            RefreshPresets();
            LoadSoundfontNow();
            Apply();
            RebuildLater();
            break;
        }
        case MSG_PRESET: {
            int32 p = 0;
            msg->FindInt32("preset", &p);
            if (p == fDesc.sf2Preset) break;
            fDesc.sf2Preset = p;
            LoadSoundfontNow();   // a preset is a separate cache entry
            Apply();
            RebuildLater();
            break;
        }
        case MSG_REBUILD:
            Build();
            break;
        case MSG_WAVE: {
            int32 wv = 0;
            msg->FindInt32("wave", &wv);
            fDesc.synth.waveform = wv;
            Apply();
            RebuildLater();   // refresh the marked item, off this dispatch
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
                case 0: fDesc.synth.attack  = val; break;
                case 1: fDesc.synth.decay   = val; break;
                case 2: fDesc.synth.sustain = val; break;
                case 3: fDesc.synth.release = val; break;
            }
            Apply();     // value only; no rebuild
            break;
        }
        default:
            BWindow::MessageReceived(msg);
    }
}

} // namespace daw
