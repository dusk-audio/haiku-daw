#include "TimelineView.h"

#include "UiMetrics.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "PianoRoll.h"
#include "SampleBrowser.h"   // kMsgSampleDrag / kMsgBrowserImport
#include "RenameWindow.h"
#include "../engine/AudioFormats.h"  // SniffAudioFileFormat (the drop filter)
#include "../engine/Recorder.h" // live capture waveform envelope
#include "../model/Crossfade.h" // effective (auto-crossfade) clip fades
#include "Widgets.h"            // shared pan knob draw

#include <Entry.h>
#include <MenuItem.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <String.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <strings.h>   // strcasecmp (dropped-file extension match)

namespace daw {

TimelineView::TimelineView(BRect frame, Project* project, CommandStack* stack)
    : BView(frame, "timeline", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE
            | B_SUPPORTS_LAYOUT),
      fProject(project),
      fStack(stack),
      fFramesPerPixel(kDefaultFramesPerPixel),
      fScrollFrame(0) {
    SetViewColor(ColBackground());
}

// --- Header control geometry (relative to a lane's top edge) ----------
static constexpr float kMaxGain = 1.5f;   // fader top of travel

// MIDI piano-roll vertical range: pitches [kMidiLow, kMidiLow+kMidiRange).
static constexpr int kMidiLow   = 36;
static constexpr int kMidiRange = 48;

// How close (px) to a block's right edge counts as a resize grab.
// How close to a region's edge counts as "grab the edge to resize".
//
// 5 px was almost impossible to hit deliberately, which made resizing look like
// it did not exist. 9 is still narrow enough that grabbing the middle of a short
// region moves it rather than resizing it.
static constexpr float kEdgeGrab = 9.0f;   // design px; use EdgeGrab()
static float EdgeGrab() { return Themed(kEdgeGrab); }

// Edits snap to this grid resolution (16th notes) unless Shift is held.
static constexpr int kSnapDivision = 4;

// Defined below; used by TrackIndexAt/LaneRect above its definition.
static float LaneHeightOf(const Track& t);

// Slim per-lane header controls: name, M/S/R + input-monitor, a pan knob, and a
// horizontal gain fader. Everything else (routing, fx, sends, instrument, input)
// lives in the left inspector for the selected track.
// Design pixels through Themed(): drawing AND hit-testing both come through
// here, so the click targets follow the scale for free.
static BRect MuteRect(BRect lane) {
    return BRect(Themed(6),   lane.top + Themed(20),
                 Themed(24),  lane.top + Themed(38));
}
static BRect SoloRect(BRect lane) {
    return BRect(Themed(28),  lane.top + Themed(20),
                 Themed(46),  lane.top + Themed(38));
}
static BRect ArmRect(BRect lane) {
    return BRect(Themed(50),  lane.top + Themed(20),
                 Themed(68),  lane.top + Themed(38));
}
static BRect MonRect(BRect lane) {
    return BRect(Themed(72),  lane.top + Themed(20),
                 Themed(90),  lane.top + Themed(38));
}
static BRect PanKnobRect(BRect lane) {
    return BRect(Themed(100), lane.top + Themed(15),
                 Themed(128), lane.top + Themed(43));
}
static BRect GainRect(BRect lane) {
    return BRect(Themed(6),   lane.top + Themed(50),
                 Themed(128), lane.top + Themed(62));
}

float TimelineView::FrameToX(Frame f) const {
    return HeaderWidth()
         + static_cast<float>((f - fScrollFrame) / fFramesPerPixel);
}

Frame TimelineView::XToFrame(float x) const {
    return fScrollFrame
         + static_cast<Frame>((x - HeaderWidth()) * fFramesPerPixel);
}

void TimelineView::AttachedToWindow() {
    MakeFocus(true);   // receive arrow/zoom keys
}

void TimelineView::ZoomBy(double factor) {
    double fpp = fFramesPerPixel * factor;
    if (fpp < 16.0)    fpp = 16.0;      // most zoomed-in
    if (fpp > 65536.0) fpp = 65536.0;   // most zoomed-out
    fFramesPerPixel = fpp;
    Invalidate();
}

void TimelineView::PanBy(Frame deltaFrames) {
    fScrollFrame += deltaFrames;
    if (fScrollFrame < 0) fScrollFrame = 0;
    // Don't scroll past the content (last clip/note end).
    Frame end = 0;
    for (const Track& t : fProject->Tracks()) {
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > end) end = c.startFrame + c.lengthFrames;
        for (const MidiClip& mc : t.midiClips)
            if (mc.startFrame + mc.lengthFrames > end) end = mc.startFrame + mc.lengthFrames;
    }
    if (fScrollFrame > end) fScrollFrame = end;
    Invalidate();
}

void TimelineView::ScrollVerticalBy(float dy) {
    const float viewH = Bounds().Height() - RulerHeight();
    const float maxScroll = ContentHeight() - viewH;
    fScrollY += dy;
    if (fScrollY > maxScroll) fScrollY = maxScroll;
    if (fScrollY < 0.0f)      fScrollY = 0.0f;   // (also clamps when all fits)
    Invalidate();
}

void TimelineView::ZoomToFit() {
    Frame end = 0;
    for (const Track& t : fProject->Tracks()) {
        for (const Clip& c : t.clips)
            if (c.startFrame + c.lengthFrames > end) end = c.startFrame + c.lengthFrames;
        for (const MidiClip& mc : t.midiClips)
            if (mc.startFrame + mc.lengthFrames > end) end = mc.startFrame + mc.lengthFrames;
    }
    const float contentW = Bounds().Width() - HeaderWidth();
    if (end <= 0 || contentW < 1.0f) return;
    double fpp = (double)end / contentW * 1.05;   // small margin
    if (fpp < 16.0)    fpp = 16.0;
    if (fpp > 65536.0) fpp = 65536.0;
    fFramesPerPixel = fpp;
    fScrollFrame = 0;
    Invalidate();
}

void TimelineView::CycleAuto(TrackId id) {
    const Track* t = fProject ? fProject->FindTrack(id) : nullptr;
    if (!t) return;
    // Off -> Gain -> Pan -> each fx-param lane -> Off (same as the old box).
    int& m = fAutoMode[id];
    m = (m + 1) % (3 + (int)t->fxAuto.size());
    Invalidate();
}

// Case-insensitive filename-extension match, used to route a dropped file to the
// audio or the MIDI importer (Tracker gives us paths, not MIME types).
static bool DroppedExtIs(const char* path, const char* ext) {
    const char* dot = std::strrchr(path, '.');
    return dot && strcasecmp(dot + 1, ext) == 0;
}

void TimelineView::MessageReceived(BMessage* msg) {
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        float dy = 0.0f;
        if (msg->FindFloat("be:wheel_delta_y", &dy) == B_OK && dy != 0.0f) {
            ScrollVerticalBy(dy * 40.0f);   // ~40 px per notch
            return;
        }
    }
    // A sample dragged from the browser + dropped here: import it onto the
    // track under the cursor at the snapped drop position.
    if (msg->what == kMsgSampleDrag && fProject) {
        const char* path = nullptr;
        if (msg->FindString("path", &path) == B_OK && path) {
            const BPoint where = ConvertFromScreen(msg->DropPoint());
            const int idx = TrackIndexAt(where);
            TrackId tid = kInvalidTrackId;
            if (idx >= 0 && idx < (int)fProject->Tracks().size())
                tid = fProject->Tracks()[(size_t)idx].id;
            Frame start = Snapped(XToFrame(where.x));
            if (start < 0) start = 0;
            BMessage imp(kMsgBrowserImport);
            imp.AddString("path", path);
            imp.AddInt64("tid", (int64)tid);
            imp.AddInt64("start", (int64)start);
            if (Window()) BMessenger(Window()).SendMessage(&imp);
        }
        return;
    }
    // Files dropped from Tracker (or any other B_SIMPLE_DATA source): import
    // each ref onto the track it landed on, at the snapped drop position.
    if (msg->what == B_SIMPLE_DATA && fProject) {
        const BPoint where = ConvertFromScreen(msg->DropPoint());
        Frame start = Snapped(XToFrame(where.x));
        if (start < 0) start = 0;
        const int dropIdx = TrackIndexAt(where);
        entry_ref ref;
        // Walks the target down consecutive tracks so a multi-file audio drop
        // lands side by side instead of stacked. Only audio advances it: a .mid
        // makes its own tracks, so counting it would skip a lane.
        int audioPlaced = 0;
        for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; i++) {
            BEntry entry(&ref, true);   // traverse symlinks
            BPath path;
            if (entry.InitCheck() != B_OK || !entry.IsFile()
                || entry.GetPath(&path) != B_OK)
                continue;
            const char* p = path.Path();

            BMessage imp;
            // Audio is decided by the file's leading bytes, not its name, so
            // every format the build can read lands here -- including a WAV
            // someone renamed and a FLAC named ".wav". MIDI has no magic of
            // its own ("MThd" does, but the reader is the authority), so the
            // extension test stays for it.
            if (SniffAudioFileFormat(p) != AudioFileFormat::Unknown) {
                TrackId tid = kInvalidTrackId;
                const int idx = dropIdx >= 0 ? dropIdx + audioPlaced : -1;
                if (idx >= 0 && idx < (int)fProject->Tracks().size())
                    tid = fProject->Tracks()[(size_t)idx].id;
                imp.what = kMsgBrowserImport;
                imp.AddString("path", p);
                imp.AddInt64("tid", (int64)tid);
                imp.AddInt64("start", (int64)start);
                ++audioPlaced;
            } else if (DroppedExtIs(p, "mid") || DroppedExtIs(p, "midi")) {
                imp.what = kMsgDropMidi;      // makes its own MIDI tracks
                imp.AddString("path", p);
                imp.AddInt64("start", (int64)start);
            } else {
                std::fprintf(stderr,
                             "TimelineView: ignoring dropped '%s' "
                             "(not an audio file or MIDI)\n", p);
                continue;
            }
            if (Window()) BMessenger(Window()).SendMessage(&imp);
        }
        return;
    }
    BView::MessageReceived(msg);
}

void TimelineView::KeyDown(const char* bytes, int32 numBytes) {
    if (numBytes < 1) { BView::KeyDown(bytes, numBytes); return; }
    // One page = the visible content width in frames.
    const Frame page = (Frame)((Bounds().right - HeaderWidth()) * fFramesPerPixel);
    switch (bytes[0]) {
        case B_LEFT_ARROW:
            if (modifiers() & B_COMMAND_KEY) JumpToMarker(-1);
            else PanBy(-page / 4);
            break;
        case B_RIGHT_ARROW:
            if (modifiers() & B_COMMAND_KEY) JumpToMarker(+1);
            else PanBy(page / 4);
            break;
        case 'l': case 'L': LoopBetweenMarkers(); break;   // cycle marker-to-marker
        case B_HOME:        fScrollFrame = 0; Invalidate(); break;
        case '+': case '=': ZoomBy(0.5); break;   // zoom in
        case '-': case '_': ZoomBy(2.0); break;   // zoom out
        case 'f': case 'F': ZoomToFit(); break;   // fit project to view width
        case B_SPACE:   // toggle transport (play/stop)
            if (BWindow* w = Window()) w->PostMessage(kMsgTransportToggle);
            break;
        case 'm': case 'M': {   // drop a marker at the playhead
            Frame at = fProject->transport.playhead; if (at < 0) at = 0;
            char nm[24];
            std::snprintf(nm, sizeof(nm), "Marker %d",
                          (int)fProject->markers.size() + 1);
            fStack->Execute(std::make_unique<AddMarkerCommand>(at, nm), *fProject);
            Invalidate();
            break;
        }
        case B_PAGE_UP:   ScrollVerticalBy(-(Bounds().Height() - RulerHeight()) * 0.8f); break;
        case B_PAGE_DOWN: ScrollVerticalBy( (Bounds().Height() - RulerHeight()) * 0.8f); break;
        case B_DELETE: case B_BACKSPACE:
            if (!fSelClips.empty()) DeleteSelection();
            else BView::KeyDown(bytes, numBytes);
            break;
        case 'd': case 'D':
            if (modifiers() & B_CONTROL_KEY) DuplicateSelection();
            else BView::KeyDown(bytes, numBytes);
            break;
        case B_ESCAPE:
            if (!fSelClips.empty()) { fSelClips.clear(); Invalidate(); }
            else BView::KeyDown(bytes, numBytes);
            break;
        default: BView::KeyDown(bytes, numBytes);
    }
}

Grid TimelineView::GridOf() const {
    Grid g;
    g.sampleRate  = fProject->sampleRate;
    g.tempoBPM    = fProject->tempoBPM;
    g.beatsPerBar = fProject->timeSig.numerator;
    return g;
}

Frame TimelineView::Snapped(Frame f) const {
    if (modifiers() & B_SHIFT_KEY)   // hold Shift for free placement
        return f;
    // Snap to the nearest beat subdivision using the tempo map (variable tempo).
    const TempoMap& tm = fProject->tempoMap;
    const double beat = tm.BeatAt(f < 0 ? 0 : f);
    const double snapped = std::llround(beat * kSnapDivision) / (double)kSnapDivision;
    const Frame out = tm.FrameAt(snapped);
    return out < 0 ? 0 : out;
}

// A tiny Copy/Delete popup for a right-clicked clip or note.
int TimelineView::ContextMenu(BPoint where, bool withSplit, bool withTake) const {
    BPopUpMenu* m = new BPopUpMenu("ctx", false, false);
    m->AddItem(new BMenuItem("Copy", NULL));      // 0
    m->AddItem(new BMenuItem("Delete", NULL));    // 1
    if (withSplit)
        m->AddItem(new BMenuItem("Split here", NULL));  // 2
    if (withTake)
        m->AddItem(new BMenuItem("Next Take", NULL));   // 3 (2 if no split)
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    const int idx = sel ? m->IndexOf(sel) : -1;
    delete m;
    return idx;
}

std::string TimelineView::AudioClipMenu(BPoint where, bool withTake) const {
    BPopUpMenu* m = new BPopUpMenu("clip", false, false);
    m->AddItem(new BMenuItem("Copy", NULL));
    m->AddItem(new BMenuItem("Delete", NULL));
    m->AddItem(new BMenuItem("Split here", NULL));
    if (withTake)
        m->AddItem(new BMenuItem("Next Take", NULL));
    m->AddSeparatorItem();
    m->AddItem(new BMenuItem("Normalize", NULL));
    m->AddItem(new BMenuItem("Reverse", NULL));
    m->AddItem(new BMenuItem("Strip Silence", NULL));
    m->AddItem(new BMenuItem("Clear Fades", NULL));
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    std::string label = sel ? std::string(sel->Label()) : std::string();
    delete m;
    return label;
}

bool TimelineView::PastePopup(BPoint where) const {
    BPopUpMenu* m = new BPopUpMenu("paste", false, false);
    m->AddItem(new BMenuItem("Paste here", NULL));
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    const bool ok = (sel != NULL);
    delete m;
    return ok;
}

void TimelineView::PasteToTrack(TrackId track, Frame at, TrackType type) {
    if (type == TrackType::Audio && fHasClipClip) {
        Clip c = fClipClip;
        c.id = kInvalidClipId;
        c.startFrame = at;
        fStack->Execute(std::make_unique<AddClipCommand>(track, c), *fProject);
        Invalidate();
    } else if (type == TrackType::Midi && fHasClipMidi) {
        MidiClip c = fClipMidi;
        c.id = kInvalidClipId;      // AddMidiClipCommand assigns a fresh id
        c.startFrame = at;
        fStack->Execute(std::make_unique<AddMidiClipCommand>(track, c), *fProject);
        Invalidate();
    }
}

// Open the piano-roll editor for one MIDI region. Notes are edited clip-relative
// and posted back scoped to `clip` (see PianoRollView::Apply / kMsgApplyNotes).
void TimelineView::OpenPianoRollForClip(TrackId track, ClipId clip) {
    Track* t = fProject->FindTrack(track);
    if (!t) return;
    if (t->FindMidiClip(clip) == nullptr) return;
    // The main window owns the editor pane (M1.4): it decides whether the
    // editor is docked in the bottom pane or popped out into its own window,
    // and it is the only place that knows where the dock is.
    if (BWindow* w = Window()) {
        BMessage m(kMsgOpenEditor);
        m.AddInt64("track", (int64)track);
        m.AddInt64("clip", (int64)clip);
        w->PostMessage(&m);
    }
}

void TimelineView::PasteAtPlayhead() {
    const Frame at = fProject->transport.playhead;
    if (fHasClipClip) {
        // Prefer a track of the source type; fall back to the first such track.
        TrackId target = kInvalidTrackId;
        for (const Track& t : fProject->Tracks())
            if (t.type == TrackType::Audio) { target = t.id; break; }
        if (target != kInvalidTrackId) {
            Clip c = fClipClip;
            c.id = kInvalidClipId;      // AddClipCommand assigns a fresh id
            c.startFrame = at;
            fStack->Execute(std::make_unique<AddClipCommand>(target, c), *fProject);
            Invalidate();
        }
    } else if (fHasClipMidi) {
        TrackId target = kInvalidTrackId;
        for (const Track& t : fProject->Tracks())
            if (t.type == TrackType::Midi) { target = t.id; break; }
        if (target != kInvalidTrackId) {
            MidiClip c = fClipMidi;
            c.id = kInvalidClipId;
            c.startFrame = at;
            fStack->Execute(std::make_unique<AddMidiClipCommand>(target, c), *fProject);
            Invalidate();
        }
    }
}

Track* TimelineView::TrackOfClip(ClipId id) const {
    for (Track& t : fProject->Tracks())
        if (t.FindClip(id) || t.FindMidiClip(id))   // audio or MIDI region
            return &t;
    return nullptr;
}

void TimelineView::DeleteSelection() {
    if (fSelClips.empty()) return;
    auto macro = std::make_unique<MacroCommand>("Delete Clips");
    for (ClipId id : fSelClips)
        if (Track* t = TrackOfClip(id)) {
            if (t->FindClip(id))
                macro->Add(std::make_unique<RemoveClipCommand>(t->id, id));
            else if (t->FindMidiClip(id))
                macro->Add(std::make_unique<RemoveMidiClipCommand>(t->id, id));
        }
    if (!macro->Empty())
        fStack->Execute(std::move(macro), *fProject);
    fSelClips.clear();
    Invalidate();
}

void TimelineView::DuplicateSelection() {
    if (fSelClips.empty()) return;
    // Offset the copies by the selection's total span so they land just after.
    bool have = false; Frame minStart = 0, maxEnd = 0;
    auto span = [&](Frame s, Frame len) {
        const Frame e = s + len;
        if (!have) { minStart = s; maxEnd = e; have = true; }
        else { if (s < minStart) minStart = s; if (e > maxEnd) maxEnd = e; }
    };
    for (ClipId id : fSelClips)
        if (Track* t = TrackOfClip(id)) {
            if (Clip* c = t->FindClip(id)) span(c->startFrame, c->lengthFrames);
            else if (MidiClip* m = t->FindMidiClip(id))
                span(m->startFrame, m->lengthFrames);
        }
    if (!have) return;
    const Frame off = maxEnd - minStart;
    auto macro = std::make_unique<MacroCommand>("Duplicate Clips");
    for (ClipId id : fSelClips)
        if (Track* t = TrackOfClip(id)) {
            if (Clip* c = t->FindClip(id)) {
                Clip nc = *c;
                nc.id = kInvalidClipId;         // AddClipCommand assigns a fresh id
                nc.startFrame = c->startFrame + off;
                macro->Add(std::make_unique<AddClipCommand>(t->id, nc));
            } else if (MidiClip* m = t->FindMidiClip(id)) {
                MidiClip nc = *m;
                nc.id = kInvalidClipId;
                nc.startFrame = m->startFrame + off;
                macro->Add(std::make_unique<AddMidiClipCommand>(t->id, nc));
            }
        }
    if (!macro->Empty())
        fStack->Execute(std::move(macro), *fProject);
    Invalidate();
}

namespace {
// Optional draw timing (DAW_TIMELINE_TIMING=1), so the plan's "under 4 ms per
// frame" claim is measured on the target rather than asserted here. Reports
// the average of every 60 draws on stderr; handles Draw()'s early returns.
struct DrawTimer {
    bigtime_t start = 0;
    bool      on    = false;
    DrawTimer() {
        static const bool enabled = std::getenv("DAW_TIMELINE_TIMING") != nullptr;
        on = enabled;
        if (on) start = system_time();
    }
    ~DrawTimer() {
        if (!on) return;
        static bigtime_t sum = 0;
        static int       n   = 0;
        sum += system_time() - start;
        if (++n >= 60) {
            std::fprintf(stderr, "timeline draw: %.2f ms average over %d draws\n",
                         (double)sum / 1000.0 / n, n);
            sum = 0;
            n = 0;
        }
    }
};
} // namespace

// The effective fades for a track, cached against the exact clip tuple they
// were computed from (ComputeCrossfades allocates; it changes only on edits).
const std::vector<ClipFades>& TimelineView::FadesFor(const Track& t) {
    FadeCache& e = fFadeCache[t.id];
    bool same = e.key.size() == t.clips.size();
    for (std::size_t i = 0; same && i < t.clips.size(); i++) {
        const Clip& c = t.clips[i];
        same = e.key[i][0] == c.startFrame && e.key[i][1] == c.lengthFrames
            && e.key[i][2] == c.fadeInFrames && e.key[i][3] == c.fadeOutFrames
            && e.key[i][4] == c.takeGroup;
    }
    if (!same) {
        e.fades = ComputeCrossfades(t.clips);
        e.key.clear();
        e.key.reserve(t.clips.size());
        for (const Clip& c : t.clips)
            e.key.push_back({ c.startFrame, c.lengthFrames, c.fadeInFrames,
                              c.fadeOutFrames, c.takeGroup });
    }
    return e.fades;
}

void TimelineView::Draw(BRect updateRect) {
    DrawTimer timer;   // no-op unless DAW_TIMELINE_TIMING is set
    DrawLanes(updateRect);
    if (fDrag == Drag::Clip && fDragCurLane >= 0)
        DrawDragGhost();     // clip-move preview
    if (fBanding) {          // rubber-band selection rectangle
        BRect b(std::min(fBandA.x, fBandB.x), std::min(fBandA.y, fBandB.y),
                std::max(fBandA.x, fBandB.x), std::max(fBandA.y, fBandB.y));
        SetHighColor(Rgb(200, 220, 255));
        StrokeRect(b);
    }
    // Loop / punch region overlays across the lanes (translucent), + marker
    // lines, so the cycle range and markers are visible in the arrangement, not
    // just on the ruler.
    const Transport& tr = fProject->transport;
    const float botY = Bounds().bottom;
    SetDrawingMode(B_OP_ALPHA);
    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
    auto band = [&](Frame a, Frame b, uint8 r, uint8 g, uint8 bl, uint8 al) {
        float x0 = FrameToX(a), x1 = FrameToX(b);
        if (x0 < HeaderWidth()) x0 = HeaderWidth();
        if (x1 <= x0) return;
        SetHighColor(r, g, bl, al);
        FillRect(BRect(x0, RulerHeight(), x1, botY));
    };
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart)
        band(tr.loopStart, tr.loopEnd, 70, 120, 95, 40);      // green
    if (tr.punchEnabled && tr.punchOut > tr.punchIn)
        band(tr.punchIn, tr.punchOut, 160, 60, 60, 45);       // red
    for (const Marker& mk : fProject->markers) {
        const float x = FrameToX(mk.frame);
        if (x < HeaderWidth() || x > Bounds().right) continue;
        SetHighColor(52, 199, 89, 70);
        StrokeLine(BPoint(x, RulerHeight()), BPoint(x, botY));
    }
    SetDrawingMode(B_OP_COPY);

    DrawPlayhead();          // over lanes, under the ruler
    DrawRuler(updateRect);   // ruler last so it sits above lane content
}

void TimelineView::DrawDragGhost() {
    if (fMultiMove) {   // one ghost per selected clip at start + delta
        SetHighColor(Rgb(210, 225, 255));
        int idx = 0;
        for (const Track& t : fProject->Tracks()) {
            const BRect lane = LaneRect(idx++);
            if (t.type != TrackType::Audio) continue;
            for (const Clip& c : t.clips) {
                if (!ClipSelected(c.id)) continue;
                Frame ns = c.startFrame + fMultiDelta;
                if (ns < 0) ns = 0;
                float x0 = FrameToX(ns), x1 = FrameToX(ns + c.lengthFrames);
                if (x0 < HeaderWidth()) x0 = HeaderWidth();
                if (x1 <= x0) continue;
                StrokeRect(BRect(x0, lane.top + Themed(3), x1, lane.bottom - Themed(3)));
            }
        }
        return;
    }
    if (fDragCurLane >= (int)fProject->Tracks().size())
        return;
    BRect lane = LaneRect(fDragCurLane);
    float x0 = FrameToX(fDragCurStart);
    float x1 = FrameToX(fDragCurStart + fDragClipOrigLen);
    if (x0 < HeaderWidth()) x0 = HeaderWidth();
    if (x1 <= x0) return;
    BRect g(x0, lane.top + Themed(3), x1, lane.bottom - Themed(3));
    SetHighColor(Rgb(210, 225, 255));
    StrokeRect(g);
    StrokeLine(BPoint(g.left, g.top + Themed(1)), BPoint(g.right, g.top + Themed(1)));
}

void TimelineView::SetPlayhead(Frame f) {
    if (f == fPlayhead)
        return;
    // Follow (chase): page the scroll when the playhead nears the right edge or
    // falls before the visible window, so a long take stays on screen.
    if (fFollow) {
        const float contentW = Bounds().Width() - HeaderWidth();
        if (contentW > 1.0f) {
            const Frame viewFrames = (Frame)(contentW * fFramesPerPixel);
            if (f < fScrollFrame
                || f > fScrollFrame + (Frame)(viewFrames * 0.9)) {
                fScrollFrame = f - (Frame)(viewFrames * 0.1);
                if (fScrollFrame < 0) fScrollFrame = 0;
                fPlayhead = f;
                Invalidate();
                return;
            }
        }
    }
    const float xOld = FrameToX(fPlayhead);
    const float xNew = FrameToX(f);
    fPlayhead = f;
    // Repaint the two 1-px columns (a hair wide for the AA'd line) from the
    // ruler bottom to the view bottom.
    BRect b = Bounds();
    Invalidate(BRect(xOld - 1, RulerHeight(), xOld + 1, b.bottom));
    Invalidate(BRect(xNew - 1, RulerHeight(), xNew + 1, b.bottom));
}

int TimelineView::TrackIndexAt(BPoint where) const {
    if (!fProject || where.y < RulerHeight())
        return -1;
    // Walk cumulative lane heights (variable per track), scroll-offset.
    float y = RulerHeight() - fScrollY;
    const auto& tracks = fProject->Tracks();
    for (int i = 0; i < (int)tracks.size(); i++) {
        const float h = LaneHeightOf(tracks[i]);
        if (where.y >= y && where.y <= y + h)
            return i;
        y += h + TrackGap();   // clicks in the gap fall through -> -1
    }
    return -1;
}

int TimelineView::PitchAt(BRect lane, float y) const {
    const float rel = (lane.bottom - y) / lane.Height();
    int pitch = kMidiLow + (int)(rel * kMidiRange + 0.5f);
    if (pitch < 0) pitch = 0;
    if (pitch > 127) pitch = 127;
    return pitch;
}

Frame TimelineView::BarStartFrameAt(Frame f) const {
    const TempoMap& tm = fProject->tempoMap;
    if (f < 0) f = 0;
    const auto& meters = tm.Meters();
    size_t seg = 0;
    for (size_t i = 0; i < meters.size(); i++)
        if (meters[i].frame <= f) seg = i; else break;
    const double segStartBeat = tm.BeatAt(meters[seg].frame);
    const int    num = meters[seg].num;
    const double into = tm.BeatAt(f) - segStartBeat;
    const double bars = std::floor(into / num + 0.5);   // nearest bar
    return tm.FrameAt(segStartBeat + bars * num);
}

void TimelineView::HandleRulerMenu(BPoint where) {
    TempoMap& tm = fProject->tempoMap;
    tm.sampleRate = fProject->sampleRate;

    // A marker near the click (within ~8 px) can be removed.
    Frame nearTempo = -1, nearMeter = -1;
    for (const TempoChange& t : tm.Tempos())
        if (t.frame > 0 && std::fabs(FrameToX(t.frame) - where.x) < Themed(8.0f))
            nearTempo = t.frame;
    for (const MeterChange& m : tm.Meters())
        if (m.frame > 0 && std::fabs(FrameToX(m.frame) - where.x) < Themed(8.0f))
            nearMeter = m.frame;

    BPopUpMenu* menu = new BPopUpMenu("tm", false, false);
    BMenu* tsub = new BMenu("Tempo change");
    const int bpms[] = { 60, 80, 90, 100, 110, 120, 130, 140, 160, 180 };
    for (int b : bpms) { char s[8]; std::snprintf(s, 8, "%d", b);
                         tsub->AddItem(new BMenuItem(s, NULL)); }
    menu->AddItem(tsub);
    BMenu* msub = new BMenu("Meter change");
    const char* mets[] = { "4/4", "3/4", "2/4", "6/8", "5/4", "7/8" };
    for (const char* mm : mets) msub->AddItem(new BMenuItem(mm, NULL));
    menu->AddItem(msub);

    // Ramp toggle for the tempo change governing the click's segment (needs a
    // following change to ramp toward, so hide it on the last segment).
    Frame clickFrame = XToFrame(where.x); if (clickFrame < 0) clickFrame = 0;
    Frame  segFrame = 0; double segBpm = tm.Tempos().front().bpm;
    bool   segRamp = false;
    for (const TempoChange& t : tm.Tempos()) {
        if (t.frame <= clickFrame) { segFrame = t.frame; segBpm = t.bpm;
                                     segRamp = t.ramp; }
        else break;
    }
    const bool segHasNext = segFrame != tm.Tempos().back().frame;
    BMenuItem* rampItem = nullptr;
    if (segHasNext) {
        rampItem = new BMenuItem("Ramp to next tempo", NULL);
        rampItem->SetMarked(segRamp);
        menu->AddItem(rampItem);
    }

    if (nearTempo >= 0 || nearMeter >= 0) {
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Remove change here", NULL));
    }

    // Markers.
    const Marker* nearMarker = MarkerAt(where);
    menu->AddSeparatorItem();
    menu->AddItem(new BMenuItem("Add Marker", NULL));
    if (nearMarker) {
        menu->AddItem(new BMenuItem("Rename Marker" B_UTF8_ELLIPSIS, NULL));
        menu->AddItem(new BMenuItem("Delete Marker", NULL));
    }

    BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
    if (sel) {
        BMenu* parent = sel->Menu();
        const char* label = sel->Label();
        if (parent == tsub) {
            const Frame at = tm.FrameAt((double)std::llround(
                tm.BeatAt(XToFrame(where.x) < 0 ? 0 : XToFrame(where.x))));
            tm.SetTempoAt(at, atof(label));
        } else if (parent == msub) {
            int n = 4, d = 4; std::sscanf(label, "%d/%d", &n, &d);
            tm.SetMeterAt(BarStartFrameAt(XToFrame(where.x)), n, d);
        } else if (rampItem && sel == rampItem) {
            tm.SetTempoAt(segFrame, segBpm, !segRamp);   // toggle ramp
        } else if (label && std::strcmp(label, "Remove change here") == 0) {
            if (nearTempo >= 0) tm.RemoveTempoAt(nearTempo);
            else if (nearMeter >= 0) tm.RemoveMeterAt(nearMeter);
        } else if (label && std::strcmp(label, "Add Marker") == 0) {
            Frame at = Snapped(XToFrame(where.x)); if (at < 0) at = 0;
            char nm[24];
            std::snprintf(nm, sizeof(nm), "Marker %d",
                          (int)fProject->markers.size() + 1);
            fStack->Execute(std::make_unique<AddMarkerCommand>(at, nm), *fProject);
        } else if (nearMarker && label
                   && std::strcmp(label, "Delete Marker") == 0) {
            fStack->Execute(std::make_unique<RemoveMarkerCommand>(
                                nearMarker->frame, nearMarker->name), *fProject);
        } else if (nearMarker && label
                   && std::strncmp(label, "Rename Marker", 13) == 0) {
            // Reuse RenameWindow; the marker frame rides in the "track" id field.
            BPoint sp = ConvertToScreen(where);
            BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
            (new RenameWindow(wr, (TrackId)nearMarker->frame,
                              nearMarker->name.c_str(), BMessenger(Window()),
                              kMsgRenameMarker))->Show();
        }
        Invalidate();
    }
    delete menu;
}

void TimelineView::MouseDown(BPoint where) {
    if (!fProject || !fStack)
        return;

    // A click makes the timeline the focused view, so the transport keys
    // (Space, arrows, Home) work without clicking twice -- and so the
    // window's key router knows a text field is no longer being edited.
    MakeFocus(true);

    // Secondary (right) button = delete the thing under the cursor.
    int32 buttons = 0;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;

    // Right-click on the ruler: tempo / meter change menu.
    if (rightClick && where.y < RulerHeight() && where.x >= HeaderWidth()) {
        HandleRulerMenu(where);
        return;
    }

    // Ruler: click seeks, drag sets a loop region. Ctrl-drag sets the punch
    // range instead. On release we decide seek-vs-drag by how far it moved.
    if (where.y < RulerHeight() && where.x >= HeaderWidth()) {
        // Left-click a marker flag: jump the playhead there.
        if (!rightClick) {
            if (const Marker* mk = MarkerAt(where)) {
                fProject->transport.playhead = mk->frame;
                if (BWindow* w = Window()) w->PostMessage(kMsgSeek);
                return;
            }
        }
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        fDrag        = (modifiers() & B_CONTROL_KEY) ? Drag::RulerPunch
                                                     : Drag::RulerLoop;
        fLoopAnchor  = f;
        fLoopDragged = false;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }

    const int idx = TrackIndexAt(where);
    if (idx < 0)
        return;
    BRect lane = LaneRect(idx);
    const Track& t = fProject->Tracks()[idx];

    if (where.x < HeaderWidth()) {
        // Any header click selects the track (drives the left inspector).
        if (fSelectedTrack != t.id) {
            fSelectedTrack = t.id;
            if (BWindow* w = Window()) {
                BMessage sel(kMsgTrackSelected);
                sel.AddInt64("track", (int64)t.id);
                w->PostMessage(&sel);
            }
            Invalidate();
        }
        // Track name strip: right-click deletes the track, double-click renames.
        if (where.y <= lane.top + 18 && where.x < 114) {
            if (rightClick) {
                BPopUpMenu* mm = new BPopUpMenu("trk", false, false);
                mm->AddItem(new BMenuItem("Move Up", NULL));
                mm->AddItem(new BMenuItem("Move Down", NULL));
                mm->AddItem(new BMenuItem("Next Color", NULL));
                mm->AddItem(new BMenuItem("Taller", NULL));
                mm->AddItem(new BMenuItem("Shorter", NULL));
                mm->AddSeparatorItem();
                BMenuItem* ssItem = new BMenuItem("Solo Safe", NULL);
                ssItem->SetMarked(t.soloSafe);
                mm->AddItem(ssItem);
                // Mute-group submenu: muting any member mutes the whole group.
                BMenu* mg = new BMenu("Mute Group");
                BMenuItem* mgNone = new BMenuItem("None", NULL);
                mgNone->SetMarked(t.muteGroup == 0);
                mg->AddItem(mgNone);
                for (int g = 1; g <= 4; g++) {
                    char lb[16];
                    std::snprintf(lb, sizeof(lb), "Group %d", g);
                    BMenuItem* gi = new BMenuItem(lb, NULL);
                    gi->SetMarked(t.muteGroup == g);
                    mg->AddItem(gi);
                }
                mm->AddItem(mg);
                mm->AddItem(new BMenuItem(t.frozen ? "Unfreeze" : "Freeze", NULL));
                mm->AddItem(new BMenuItem("Rename" B_UTF8_ELLIPSIS, NULL));
                mm->AddItem(new BMenuItem("Delete", NULL));
                BMenuItem* sel = mm->Go(ConvertToScreen(where), false, true);
                const std::string pick = sel ? std::string(sel->Label())
                                             : std::string();
                delete mm;
                // Color/height/reorder route through the command stack (undoable).
                if (pick == "Move Up")
                    fStack->Execute(std::make_unique<MoveTrackCommand>(t.id, -1),
                                    *fProject);
                else if (pick == "Move Down")
                    fStack->Execute(std::make_unique<MoveTrackCommand>(t.id, +1),
                                    *fProject);
                else if (pick == "Next Color") {
                    if (Track* tr = fProject->FindTrack(t.id))
                        fStack->Execute(std::make_unique<SetTrackColorCommand>(
                            t.id, (tr->colorIndex + 1) % kTrackColorCount),
                            *fProject);
                } else if (pick == "Taller") {
                    if (Track* tr = fProject->FindTrack(t.id))
                        fStack->Execute(std::make_unique<SetTrackHeightCommand>(
                            t.id, tr->height + 24 > 300 ? 300 : tr->height + 24),
                            *fProject);
                } else if (pick == "Shorter") {
                    if (Track* tr = fProject->FindTrack(t.id))
                        fStack->Execute(std::make_unique<SetTrackHeightCommand>(
                            t.id, tr->height - 24 < 72 ? 72 : tr->height - 24),
                            *fProject);
                } else if (pick == "Solo Safe") {
                    // Project state -- it serializes -- so through the stack like
                    // every other edit: it undoes now, and it marks the project
                    // dirty, which the old direct write never did.
                    if (Track* tr = fProject->FindTrack(t.id))
                        fStack->Execute(std::make_unique<SetSoloSafeCommand>(
                            t.id, !tr->soloSafe), *fProject);
                    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
                } else if (pick == "None" || pick.rfind("Group ", 0) == 0) {
                    const int g = pick == "None" ? 0 : std::atoi(pick.c_str() + 6);
                    fStack->Execute(std::make_unique<SetTrackMuteGroupCommand>(
                        t.id, g), *fProject);
                    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
                } else if (pick == "Freeze" || pick == "Unfreeze") {
                    // Freeze needs an offline render (file I/O): hand off to the
                    // main window. Unfreeze is pure model but routed the same way
                    // so the main window can rebuild peaks/engine after either.
                    BMessage req(kMsgFreezeTrack);
                    req.AddInt64("track", (int64)t.id);
                    req.AddBool("freeze", pick == "Freeze");
                    if (BWindow* w = Window()) w->PostMessage(&req);
                } else if (pick == "Rename" B_UTF8_ELLIPSIS) {
                    BPoint sp = ConvertToScreen(where);
                    BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
                    (new RenameWindow(wr, t.id, t.name.c_str(),
                                      BMessenger(Window())))->Show();
                } else if (pick == "Delete")
                    fStack->Execute(std::make_unique<RemoveTrackCommand>(t.id),
                                    *fProject);
                Invalidate();
                return;
            }
            int32 clicks = 1;
            if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
                m->FindInt32("clicks", &clicks);
            if (clicks >= 2) {   // double-click a track name: rename it
                BPoint sp = ConvertToScreen(where);
                BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
                (new RenameWindow(wr, t.id, t.name.c_str(),
                                  BMessenger(Window())))->Show();
                return;
            }
        }
        HandleHeaderClick(t, lane, where);
        return;
    }

    // Automation mode: the content area edits the active gain/pan curve instead
    // of clips/notes. Cycle the header "Auto" box back to Off to edit clips.
    {
        auto am = fAutoMode.find(t.id);
        if (am != fAutoMode.end() && am->second != 0) {
            HandleAutoMouseDown(t, lane, idx, where, rightClick);
            return;
        }
    }

    // MIDI track content: regions behave like audio clips. Double-click opens
    // the piano roll (creating a region first on empty space); drag moves or
    // (right edge) resizes; right-click = Copy/Delete/Paste. Notes are edited
    // in the piano roll, not here.
    if (t.type == TrackType::Midi) {
        const Frame at = XToFrame(where.x);
        int hit = -1;
        for (size_t i = 0; i < t.midiClips.size(); i++) {
            const MidiClip& c = t.midiClips[i];
            if (c.takeGroup > 0 && !c.takeActive)
                continue;   // inactive takes are hidden; act on the visible one
            if (at >= c.startFrame && at < c.startFrame + c.lengthFrames) {
                hit = (int)i; break;
            }
        }
        int32 clicks = 1;
        if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
            m->FindInt32("clicks", &clicks);

        if (rightClick) {
            if (hit >= 0) {
                const MidiClip& c = t.midiClips[(size_t)hit];
                const int pick = ContextMenu(where, /*withSplit=*/true,
                                             /*withTake=*/c.takeGroup > 0);
                if (pick == 0) {          // Copy
                    fClipMidi = c; fHasClipMidi = true;
                    fHasClipClip = false; fHasClipNote = false;
                    fClipType = TrackType::Midi;
                } else if (pick == 1) {   // Delete
                    fStack->Execute(std::make_unique<RemoveMidiClipCommand>(
                        t.id, c.id), *fProject);
                } else if (pick == 2) {   // Split here
                    Frame sat = Snapped(XToFrame(where.x));
                    fStack->Execute(std::make_unique<SplitMidiClipCommand>(
                        t.id, c.id, sat), *fProject);
                } else if (pick == 3 && c.takeGroup > 0) {   // Next Take
                    // Cycle to the next take in the group (by startFrame origin
                    // they all share, so order by id), wrapping to the first.
                    ClipId next = kInvalidClipId; ClipId bestAbove = 0;
                    ClipId firstId = kInvalidClipId; ClipId firstKey = 0;
                    bool haveFirst = false;
                    for (const MidiClip& o : t.midiClips) {
                        if (o.takeGroup != c.takeGroup) continue;
                        if (!haveFirst || o.id < firstKey) {
                            firstKey = o.id; firstId = o.id; haveFirst = true;
                        }
                        if (o.id > c.id
                            && (next == kInvalidClipId || o.id < bestAbove)) {
                            bestAbove = o.id; next = o.id;
                        }
                    }
                    if (next == kInvalidClipId) next = firstId;   // wrap
                    if (next != kInvalidClipId)
                        fStack->Execute(std::make_unique<SetActiveMidiTakeCommand>(
                            t.id, next), *fProject);
                }
            } else if (fHasClipMidi) {     // empty lane: offer paste
                if (PastePopup(where))
                    PasteToTrack(t.id, Snapped(XToFrame(where.x)), TrackType::Midi);
            }
            Invalidate(lane);
            return;
        }

        if (clicks >= 2) {   // open the piano roll (create a region on empty)
            ClipId cid = kInvalidClipId;
            if (hit >= 0) {
                cid = t.midiClips[(size_t)hit].id;
            } else {
                Frame st = Snapped(XToFrame(where.x));
                if (st < 0) st = 0;
                MidiClip nc;
                nc.startFrame = st;
                nc.lengthFrames = fProject->tempoMap.FrameAt(
                                      fProject->tempoMap.BeatAt(st) + 4) - st;  // ~1 bar
                if (nc.lengthFrames < 1)
                    nc.lengthFrames = (Frame)(fProject->tempoMap.FramesPerBeatAt(st) * 4);
                auto add = std::make_unique<AddMidiClipCommand>(t.id, nc);
                AddMidiClipCommand* ap = add.get();
                fStack->Execute(std::move(add), *fProject);
                cid = ap->CreatedId();
                Invalidate(lane);
            }
            if (cid != kInvalidClipId) OpenPianoRollForClip(t.id, cid);
            return;
        }

        if (hit >= 0) {   // select + begin move/resize
            const MidiClip& c = t.midiClips[(size_t)hit];
            if (modifiers() & B_SHIFT_KEY) {
                if (ClipSelected(c.id)) fSelClips.erase(c.id);
                else                    fSelClips.insert(c.id);
                Invalidate(lane);
                return;
            }
            if (!ClipSelected(c.id)) { fSelClips.clear(); fSelClips.insert(c.id); }
            fDragTrack       = t.id;
            fDragLane        = idx;
            fDragClip        = c.id;
            fDragClipOrig    = c.startFrame;
            fDragClipOrigLen = c.lengthFrames;
            fDragClipSrcOrig = 0;              // MIDI regions have no source
            fDragFadeInOrig  = c.fadeInFrames;
            fDragFadeOutOrig = c.fadeOutFrames;
            fDragIsMidiClip  = true;
            const float xStart = FrameToX(c.startFrame);
            const float xEnd   = FrameToX(c.startFrame + c.lengthFrames);
            const bool  wide   = (xEnd - xStart) > 2 * EdgeGrab();
            // Fade grips: the top name-strip band within 14px of either edge
            // (checked before move/resize so the corner always grabs the fade).
            const bool  topBand = where.y <= lane.top + Themed(16);
            if (topBand && where.x <= xStart + Themed(14)) {
                fDrag = Drag::ClipFadeIn;    // top-left grip = fade in
            } else if (topBand && where.x >= xEnd - Themed(14)) {
                fDrag = Drag::ClipFadeOut;   // top-right grip = fade out
            } else if (wide && where.x >= xEnd - EdgeGrab()) {
                fDrag = Drag::ClipResize;
            } else if (wide && where.x <= xStart + EdgeGrab()) {
                fDrag = Drag::ClipResizeLeft;
            } else {
                fDrag = Drag::Clip;
                fDragGrabOffset = XToFrame(where.x) - c.startFrame;
                // Group move: snapshot every selected MIDI region's start so a
                // drag shifts them all by one delta (same-track).
                fMultiMove = fSelClips.size() > 1;
                fMidiMoveOrig.clear();
                if (fMultiMove)
                    for (ClipId id : fSelClips)
                        if (Track* tr = TrackOfClip(id))
                            if (MidiClip* mc = tr->FindMidiClip(id))
                                fMidiMoveOrig[id] = mc->startFrame;
            }
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
        // Empty single click: clear the selection.
        if (!fSelClips.empty()) { fSelClips.clear(); Invalidate(lane); }
        return;
    }

    // Content area: right-click deletes the clip under the cursor; otherwise
    // start dragging it.
    const Frame at = XToFrame(where.x);
    for (const Clip& c : t.clips) {
        if (c.takeGroup > 0 && !c.takeActive)
            continue;   // inactive takes are hidden; act on the visible one
        if (at >= c.startFrame && at < c.startFrame + c.lengthFrames) {
            if (rightClick) {
                const std::string pick =
                    AudioClipMenu(where, /*withTake=*/c.takeGroup > 0);
                if (pick == "Copy") {
                    fClipClip = c;
                    fHasClipClip = true; fHasClipNote = false;
                    fHasClipMidi = false;   // audio copy invalidates MIDI clipboard
                    fClipType = TrackType::Audio;
                } else if (pick == "Delete") {
                    fStack->Execute(std::make_unique<RemoveClipCommand>(t.id, c.id),
                                    *fProject);
                } else if (pick == "Split here") {
                    Frame sat = Snapped(XToFrame(where.x));
                    fStack->Execute(std::make_unique<SplitClipCommand>(
                        t.id, c.id, sat), *fProject);
                } else if (pick == "Normalize" || pick == "Reverse"
                           || pick == "Strip Silence") {
                    // Needs the decoded source: hand off to the main window,
                    // which owns file I/O and issues the resulting command(s).
                    uint32 what = pick == "Normalize" ? kMsgRegionNormalize
                                : pick == "Reverse"   ? kMsgRegionReverse
                                                      : kMsgRegionStrip;
                    BMessage req(what);
                    req.AddInt64("track", (int64)t.id);
                    req.AddInt64("clip",  (int64)c.id);
                    if (BWindow* w = Window()) w->PostMessage(&req);
                } else if (pick == "Clear Fades") {
                    fStack->Execute(std::make_unique<SetClipFadeCommand>(
                        t.id, c.id, 0, 0), *fProject);
                } else if (pick == "Next Take" && c.takeGroup > 0) {   // Next Take
                    // Cycle to the next take (by sourceOffset order, wrapping).
                    ClipId next = kInvalidClipId; Frame bestAbove = 0;
                    ClipId firstId = kInvalidClipId; Frame firstOff = 0;
                    bool haveFirst = false;
                    for (const Clip& o : t.clips) {
                        if (o.takeGroup != c.takeGroup) continue;
                        if (!haveFirst || o.sourceOffset < firstOff) {
                            firstOff = o.sourceOffset; firstId = o.id; haveFirst = true;
                        }
                        if (o.sourceOffset > c.sourceOffset
                            && (next == kInvalidClipId || o.sourceOffset < bestAbove)) {
                            bestAbove = o.sourceOffset; next = o.id;
                        }
                    }
                    if (next == kInvalidClipId) next = firstId;   // wrap
                    if (next != kInvalidClipId)
                        fStack->Execute(std::make_unique<SetActiveTakeCommand>(
                            t.id, next), *fProject);
                }
                Invalidate(lane);
                return;
            }
            // Selection: Shift-click toggles; a plain click on an unselected
            // clip selects only it (a click on an already-selected clip keeps
            // the whole selection so it can be dragged as a group).
            if (modifiers() & B_SHIFT_KEY) {
                if (ClipSelected(c.id)) fSelClips.erase(c.id);
                else                    fSelClips.insert(c.id);
                Invalidate(lane);
                return;
            }
            if (!ClipSelected(c.id)) {
                fSelClips.clear();
                fSelClips.insert(c.id);
            }
            fMultiMove       = false;
            fDragTrack       = t.id;
            fDragLane        = idx;
            fDragClip        = c.id;
            fDragClipOrig    = c.startFrame;
            fDragClipOrigLen = c.lengthFrames;
            fDragClipSrcOrig = c.sourceOffset;
            fDragFadeInOrig  = c.fadeInFrames;
            fDragFadeOutOrig = c.fadeOutFrames;
            const float xStart = FrameToX(c.startFrame);
            const float xEnd   = FrameToX(c.startFrame + c.lengthFrames);
            const bool  wide   = (xEnd - xStart) > 2 * EdgeGrab();
            const bool  topBand = where.y <= lane.top + Themed(14);
            if (modifiers() & B_CONTROL_KEY) {
                fDrag = Drag::ClipGain;        // Ctrl-drag vertical = clip gain
                fDragOrig = c.gain;
            } else if (topBand && where.x <= xStart + Themed(12)) {
                fDrag = Drag::ClipFadeIn;      // top-left corner = fade in
            } else if (topBand && where.x >= xEnd - Themed(12)) {
                fDrag = Drag::ClipFadeOut;     // top-right corner = fade out
            } else if (wide && where.x >= xEnd - EdgeGrab()) {
                fDrag = Drag::ClipResize;
            } else if (wide && where.x <= xStart + EdgeGrab()) {
                fDrag = Drag::ClipResizeLeft;
            } else {
                fDrag = Drag::Clip;
                fDragGrabOffset = at - c.startFrame;
                fDragCurLane  = idx;
                fDragCurStart = c.startFrame;
                fMultiMove    = fSelClips.size() > 1;   // drag the group together
                fMultiDelta   = 0;
            }
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            break;
        }
    }

    // Right-click on an empty part of an audio lane: offer paste here.
    if (fDrag == Drag::None && rightClick && fHasClipClip) {
        if (PastePopup(where))
            PasteToTrack(t.id, Snapped(XToFrame(where.x)), TrackType::Audio);
        return;
    }

    // Left-click on empty audio content: clear selection (unless Shift) and
    // begin a rubber-band box select.
    if (fDrag == Drag::None && !rightClick && where.x >= HeaderWidth()) {
        if (!(modifiers() & B_SHIFT_KEY)) fSelClips.clear();
        fBanding = true;
        fBandA = fBandB = where;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        Invalidate();
    }
}

void TimelineView::HandleHeaderClick(const Track& t, BRect lane, BPoint where) {
    const TrackId id = t.id;

    // Mute / solo: immediate toggle commands.
    if (MuteRect(lane).Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackMuteCommand>(id, !t.muted),
                        *fProject);
        Invalidate(lane);
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
        return;
    }
    if (SoloRect(lane).Contains(where)) {
        fStack->Execute(std::make_unique<SetTrackSoloCommand>(id, !t.soloed),
                        *fProject);
        Invalidate(lane);
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
        return;
    }
    if (MonRect(lane).Contains(where)) {
        // Per-track input monitor: hear this track's input live without arming.
        if (Track* tr = fProject->FindTrack(id)) tr->inputMonitor = !tr->inputMonitor;
        Invalidate(lane);
        if (BWindow* w = Window()) {
            w->PostMessage(kMsgMonitorRefresh);
            w->PostMessage(kMsgUiRefresh);
        }
        return;
    }
    if (ArmRect(lane).Contains(where)) {
        // Arm is transient transport state, not an undoable document edit:
        // toggle it directly. Multiple tracks may be armed; recording writes
        // one take from the single input and drops it on every armed track.
        if (Track* tr = fProject->FindTrack(id))
            tr->armed = !tr->armed;
        Invalidate(lane);
        // Arming an MIDI track with an input starts/stops idle monitoring.
        if (BWindow* w = Window()) {
            w->PostMessage(kMsgMonitorRefresh);
            w->PostMessage(kMsgUiRefresh);
        }
        return;
    }

    // Gain / pan: begin a drag. Grab the pointer so we keep getting move/up
    // events even if the cursor leaves the fader.
    Drag mode = Drag::None;
    if (GainRect(lane).Contains(where))         mode = Drag::Gain;
    else if (PanKnobRect(lane).Contains(where)) mode = Drag::Pan;
    if (mode == Drag::None)
        return;

    fDragLane  = fProject->IndexOfTrack(id);   // stable across variable heights
    fDrag      = mode;
    fDragTrack = id;
    fDragOrig  = (mode == Drag::Gain) ? t.gain : t.pan;
    fDragGrabY = where.y;                       // pan knob drags vertically
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    PreviewDrag(where);
}

// Compute the dragged value from the cursor x and write it straight into the
// model for live feedback. This is a transient preview only; the undoable
// command is pushed in MouseUp.
void TimelineView::PreviewDrag(BPoint where) {
    // Ruler loop / punch drags aren't tied to a track. Ignore sub-threshold
    // jitter so a plain click (which may wobble a frame during button-down)
    // still seeks on release instead of creating a 1-frame loop region.
    if (fDrag == Drag::RulerLoop || fDrag == Drag::RulerPunch) {
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        Transport& tr = fProject->transport;
        const Frame lo = fLoopAnchor < f ? fLoopAnchor : f;
        const Frame hi = fLoopAnchor < f ? f : fLoopAnchor;
        const Frame minDrag = (Frame)(4.0 * fFramesPerPixel);   // ~4 px
        if (hi - lo <= minDrag) return;   // not a real drag yet
        fLoopDragged = true;
        if (fDrag == Drag::RulerPunch) {
            tr.punchIn = lo; tr.punchOut = hi; tr.punchEnabled = true;
        } else {
            tr.loopStart = lo; tr.loopEnd = hi; tr.loopEnabled = true;
        }
        Invalidate(BRect(0, 0, Bounds().right, RulerHeight()));
        return;
    }

    Track* t = fProject->FindTrack(fDragTrack);
    if (!t) return;
    BRect lane = LaneRect(fDragLane);

    if (fDrag == Drag::Gain) {
        BRect g = GainRect(lane);
        float v = (where.x - g.left) / g.Width() * kMaxGain;
        if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
        t->gain = v;
    } else if (fDrag == Drag::Pan) {
        // Pan knob: vertical drag (up = right), relative to the grab point.
        float v = fDragOrig + (fDragGrabY - where.y) / 100.0f;
        if (v < -1) v = -1; if (v > 1) v = 1;
        t->pan = v;
    } else if (fDrag == Drag::Clip && fDragIsMidiClip) {
        // MIDI region move: preview live (same track), commit on drop. When
        // multiple are selected, shift them all by the grabbed clip's delta
        // from each captured origin.
        Frame start = Snapped(XToFrame(where.x) - fDragGrabOffset);
        if (start < 0) start = 0;
        fDragCurStart = start;
        if (fMultiMove) {
            const Frame delta = start - fDragClipOrig;
            for (const auto& kv : fMidiMoveOrig)
                if (Track* tr = TrackOfClip(kv.first))
                    if (MidiClip* mc = tr->FindMidiClip(kv.first)) {
                        Frame ns = kv.second + delta;
                        mc->startFrame = ns < 0 ? 0 : ns;
                    }
            Invalidate();
        } else if (MidiClip* c = t->FindMidiClip(fDragClip)) {
            c->startFrame = start;
        }
    } else if (fDrag == Drag::Clip) {
        // Ghost preview: track the position + target lane; don't touch the
        // model until drop.
        Frame start = Snapped(XToFrame(where.x) - fDragGrabOffset);
        if (start < 0) start = 0;
        fDragCurStart = start;
        fMultiDelta   = fDragCurStart - fDragClipOrig;   // group shift
        const int dstIdx = TrackIndexAt(where);
        if (!fMultiMove && dstIdx >= 0
            && fProject->Tracks()[dstIdx].type == TrackType::Audio)
            fDragCurLane = dstIdx;   // cross-track move only for a single clip
        Invalidate();
        return;   // ghost is drawn in Draw(); no per-lane model change
    } else if (fDrag == Drag::ClipResizeLeft) {
        // Dragging the LEFT edge moves the start and shortens/lengthens by the
        // same amount, so the region's right edge stays put -- which is what
        // makes it a trim rather than a move. Clamped at 0 and at one frame
        // before the end, so the region can never invert or vanish.
        const Frame endFrame = fDragClipOrig + fDragClipOrigLen;
        Frame start = Snapped(XToFrame(where.x));
        if (start < 0) start = 0;
        if (start > endFrame - 1) start = endFrame - 1;
        if (fDragIsMidiClip) {
            if (MidiClip* c = t->FindMidiClip(fDragClip)) {
                // Rebase exactly as TrimClipFrontCommand::Do will, so the notes
                // stay anchored to the timeline THROUGHOUT the drag. Previewing
                // without it and rebasing only at the commit makes the notes
                // slide under the cursor and then snap back on mouse-up.
                RebaseMidiContent(*c, start - c->startFrame);
                c->startFrame   = start;
                c->lengthFrames = endFrame - start;
            }
        } else if (Clip* c = t->FindClip(fDragClip)) {
            // An audio clip also carries its read offset into the source, so
            // trimming the front must advance it by the same delta -- otherwise
            // the audio slides against the timeline instead of being trimmed.
            const Frame delta = start - fDragClipOrig;
            c->startFrame   = start;
            c->lengthFrames = endFrame - start;
            const Frame off = fDragClipSrcOrig + delta;
            c->sourceOffset = off > 0 ? off : 0;
        }
    } else if (fDrag == Drag::ClipResize && fDragIsMidiClip) {
        MidiClip* c = t->FindMidiClip(fDragClip);
        if (c) {
            Frame len = Snapped(XToFrame(where.x)) - c->startFrame;
            if (len < 1) len = 1;
            c->lengthFrames = len;
        }
    } else if (fDrag == Drag::ClipResize) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame len = Snapped(XToFrame(where.x)) - c->startFrame;
            if (len < 1) len = 1;
            c->lengthFrames = len;
        }
    } else if (fDrag == Drag::ClipFadeIn && fDragIsMidiClip) {
        MidiClip* c = t->FindMidiClip(fDragClip);
        if (c) {
            Frame f = XToFrame(where.x) - c->startFrame;
            if (f < 0) f = 0; if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeInFrames = f;
        }
    } else if (fDrag == Drag::ClipFadeOut && fDragIsMidiClip) {
        MidiClip* c = t->FindMidiClip(fDragClip);
        if (c) {
            Frame f = (c->startFrame + c->lengthFrames) - XToFrame(where.x);
            if (f < 0) f = 0; if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeOutFrames = f;
        }
    } else if (fDrag == Drag::ClipFadeIn) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame f = XToFrame(where.x) - c->startFrame;
            if (f < 0) f = 0;
            if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeInFrames = f;
        }
    } else if (fDrag == Drag::ClipFadeOut) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            Frame f = (c->startFrame + c->lengthFrames) - XToFrame(where.x);
            if (f < 0) f = 0;
            if (f > c->lengthFrames) f = c->lengthFrames;
            c->fadeOutFrames = f;
        }
    } else if (fDrag == Drag::ClipGain) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            // Vertical position over the lane maps to 0..kMaxGain (top = max).
            float f = (lane.bottom - where.y) / lane.Height();
            float v = f * kMaxGain;
            if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
            c->gain = v;
        }
    }
    Invalidate(lane);
}

void TimelineView::MouseMoved(BPoint where, uint32, const BMessage*) {
    if (fBanding) {
        fBandB = where;
        Invalidate();
        return;
    }
    if (fAutoDragging) {
        Track* t = fProject->FindTrack(fAutoTrack);
        if (!t) return;
        const int mode = fAutoMode[fAutoTrack];
        AutomationLane* live = (fAutoFx >= 0)
            ? (fAutoFx < (int)t->fxAuto.size() ? &t->fxAuto[(size_t)fAutoFx].lane
                                               : nullptr)
            : (mode == 2 ? &t->panAuto : &t->gainAuto);
        if (!live) return;
        int i = -1;
        for (size_t k = 0; k < live->Count(); k++)
            if (live->At(k).frame == fAutoDragFrame) { i = (int)k; break; }
        if (i < 0) return;
        const BRect lane = LaneRect(fDragLane);
        Frame nf = Snapped(XToFrame(where.x));
        if (nf < 0) nf = 0;
        const float nv = AutoYToValue(lane, fAutoMn, fAutoMx, where.y);
        live->RemovePoint((size_t)i);
        live->AddPoint(nf, nv);        // re-sorts; frame-key stays unique
        fAutoDragFrame = nf;
        Invalidate(lane);
        return;
    }
    if (fDrag != Drag::None)
        PreviewDrag(where);
}

void TimelineView::MouseUp(BPoint where) {
    if (fBanding) {
        fBanding = false;
        // Select every audio clip whose block intersects the band rectangle.
        BRect band(std::min(fBandA.x, fBandB.x), std::min(fBandA.y, fBandB.y),
                   std::max(fBandA.x, fBandB.x), std::max(fBandA.y, fBandB.y));
        int idx = 0;
        for (const Track& t : fProject->Tracks()) {
            const BRect lane = LaneRect(idx++);
            if (t.type != TrackType::Audio) continue;
            if (lane.bottom < band.top || lane.top > band.bottom) continue;
            for (const Clip& c : t.clips) {
                const float cx0 = FrameToX(c.startFrame);
                const float cx1 = FrameToX(c.startFrame + c.lengthFrames);
                if (cx1 >= band.left && cx0 <= band.right)
                    fSelClips.insert(c.id);
            }
        }
        Invalidate();
        return;
    }
    if (fAutoDragging) {
        fAutoDragging = false;
        Track* t = fProject->FindTrack(fAutoTrack);
        if (t) {
            const int mode = fAutoMode[fAutoTrack];
            AutomationLane* live = (fAutoFx >= 0)
                ? (fAutoFx < (int)t->fxAuto.size() ? &t->fxAuto[(size_t)fAutoFx].lane
                                                   : nullptr)
                : (mode == 2 ? &t->panAuto : &t->gainAuto);
            if (live) {
                AutomationLane edited = *live;   // edited result
                *live = fAutoOrig;               // restore pre-gesture state
                CommitAuto(fAutoTrack, mode, fAutoFx, edited);   // one undo step
            }
        }
        Invalidate();
        return;
    }
    if (fDrag == Drag::None)
        return;

    // Ruler: a real drag leaves a loop region; a bare click (no movement)
    // seeks and clears any loop — regardless of prior loop state.
    if (fDrag == Drag::RulerLoop) {
        Transport& tr = fProject->transport;
        if (!fLoopDragged) {
            tr.loopEnabled = false;
            tr.playhead = fLoopAnchor;
            SetPlayhead(fLoopAnchor);
            if (BWindow* w = Window())
                w->PostMessage(kMsgSeek);
        }
        fDrag = Drag::None;
        Invalidate();
        return;
    }
    // Punch drag: a bare click (no movement) clears the punch range.
    if (fDrag == Drag::RulerPunch) {
        if (!fLoopDragged)
            fProject->transport.punchEnabled = false;
        fDrag = Drag::None;
        Invalidate();
        return;
    }

    Track* t = fProject->FindTrack(fDragTrack);
    if (t) {
        // Restore the pre-drag value, then apply the whole gesture as one
        // undoable command (which records the correct "old" value itself).
        // Only push a command when the gesture actually changed something, so
        // a bare click (down + up, no drag) doesn't pollute the undo stack.
        std::unique_ptr<Command> cmd;
        if (fDrag == Drag::Gain) {
            const float v = t->gain;
            t->gain = fDragOrig;
            if (v != fDragOrig)
                cmd = std::make_unique<SetTrackGainCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Pan) {
            const float v = t->pan;
            t->pan = fDragOrig;
            if (v != fDragOrig)
                cmd = std::make_unique<SetTrackPanCommand>(fDragTrack, v);
        } else if (fDrag == Drag::Clip && fDragIsMidiClip && fMultiMove) {
            // MIDI group move: restore every selected region to its captured
            // origin, then commit one macro shifting all by the grabbed delta.
            const Frame delta = fDragCurStart - fDragClipOrig;
            if (delta != 0) {
                auto macro = std::make_unique<MacroCommand>("Move MIDI Clips");
                for (const auto& kv : fMidiMoveOrig)
                    if (Track* tr = TrackOfClip(kv.first))
                        if (MidiClip* mc = tr->FindMidiClip(kv.first)) {
                            mc->startFrame = kv.second;   // restore origin
                            Frame ns = kv.second + delta;
                            if (ns < 0) ns = 0;
                            macro->Add(std::make_unique<MoveMidiClipCommand>(
                                tr->id, kv.first, ns));
                        }
                if (!macro->Empty()) cmd = std::move(macro);
            }
        } else if (fDrag == Drag::Clip && fMultiMove) {
            // Group move: shift every selected clip by the same frame delta
            // (same track each), as one undoable step.
            if (fMultiDelta != 0) {
                auto macro = std::make_unique<MacroCommand>("Move Clips");
                for (ClipId id : fSelClips)
                    if (Track* tr = TrackOfClip(id))
                        if (Clip* c = tr->FindClip(id)) {
                            Frame ns = c->startFrame + fMultiDelta;
                            if (ns < 0) ns = 0;
                            macro->Add(std::make_unique<MoveClipCommand>(
                                tr->id, id, ns));
                        }
                if (!macro->Empty()) cmd = std::move(macro);
            }
        } else if (fDrag == Drag::Clip && fDragIsMidiClip) {
            // MIDI region (single): restore the previewed start, commit one move.
            if (MidiClip* c = t->FindMidiClip(fDragClip)) {
                const Frame v = fDragCurStart;
                c->startFrame = fDragClipOrig;
                if (v != fDragClipOrig)
                    cmd = std::make_unique<MoveMidiClipCommand>(
                            fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::Clip) {
            // Model was never mutated during the drag (ghost preview). Apply
            // the drop: same lane -> reposition; different lane -> move track.
            const Frame v = fDragCurStart;
            TrackId dstId = fDragTrack;
            if (fDragCurLane >= 0
                && fDragCurLane < (int)fProject->Tracks().size()
                && fProject->Tracks()[fDragCurLane].type == TrackType::Audio)
                dstId = fProject->Tracks()[fDragCurLane].id;
            if (dstId != fDragTrack)
                cmd = std::make_unique<MoveClipToTrackCommand>(
                    fDragTrack, fDragClip, dstId, v);
            else if (v != fDragClipOrig)
                cmd = std::make_unique<MoveClipCommand>(fDragTrack, fDragClip, v);
        } else if (fDrag == Drag::ClipResizeLeft) {
            // Restore the pre-drag geometry, then push ONE command carrying the
            // dragged result, the same shape every other drag here commits in.
            if (fDragIsMidiClip) {
                if (MidiClip* c = t->FindMidiClip(fDragClip)) {
                    const Frame st = c->startFrame, len = c->lengthFrames;
                    // Undo the preview's rebase along with its geometry: the
                    // command re-applies it from the pre-drag state, and
                    // snapshots the notes for its own Undo while doing so.
                    RebaseMidiContent(*c, fDragClipOrig - st);
                    c->startFrame = fDragClipOrig; c->lengthFrames = fDragClipOrigLen;
                    if (st != fDragClipOrig || len != fDragClipOrigLen)
                        cmd = std::make_unique<TrimClipFrontCommand>(
                                fDragTrack, fDragClip, true, st, len, 0);
                }
            } else if (Clip* c = t->FindClip(fDragClip)) {
                const Frame st = c->startFrame, len = c->lengthFrames;
                const Frame src = c->sourceOffset;
                c->startFrame = fDragClipOrig; c->lengthFrames = fDragClipOrigLen;
                c->sourceOffset = fDragClipSrcOrig;
                if (st != fDragClipOrig || len != fDragClipOrigLen)
                    cmd = std::make_unique<TrimClipFrontCommand>(
                            fDragTrack, fDragClip, false, st, len, src);
            }
        } else if (fDrag == Drag::ClipResize && fDragIsMidiClip) {
            if (MidiClip* c = t->FindMidiClip(fDragClip)) {
                const Frame v = c->lengthFrames;
                c->lengthFrames = fDragClipOrigLen;
                if (v != fDragClipOrigLen)
                    cmd = std::make_unique<ResizeMidiClipCommand>(
                            fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::ClipResize) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->lengthFrames;
                c->lengthFrames = fDragClipOrigLen;
                if (v != fDragClipOrigLen)
                    cmd = std::make_unique<ResizeClipCommand>(fDragTrack, fDragClip, v);
            }
        } else if (fDrag == Drag::ClipGain) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const float v = c->gain;
                c->gain = fDragOrig;
                if (v != fDragOrig)
                    cmd = std::make_unique<SetClipGainCommand>(fDragTrack,
                            fDragClip, v);
            }
        } else if ((fDrag == Drag::ClipFadeIn || fDrag == Drag::ClipFadeOut)
                   && fDragIsMidiClip) {
            if (MidiClip* c = t->FindMidiClip(fDragClip)) {
                const Frame fin = c->fadeInFrames, fout = c->fadeOutFrames;
                c->fadeInFrames = fDragFadeInOrig;
                c->fadeOutFrames = fDragFadeOutOrig;
                if (fin != fDragFadeInOrig || fout != fDragFadeOutOrig)
                    cmd = std::make_unique<SetMidiClipFadeCommand>(fDragTrack,
                            fDragClip, fin, fout);
            }
        } else if (fDrag == Drag::ClipFadeIn || fDrag == Drag::ClipFadeOut) {
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame fin = c->fadeInFrames, fout = c->fadeOutFrames;
                c->fadeInFrames = fDragFadeInOrig;
                c->fadeOutFrames = fDragFadeOutOrig;
                if (fin != fDragFadeInOrig || fout != fDragFadeOutOrig)
                    cmd = std::make_unique<SetClipFadeCommand>(fDragTrack,
                            fDragClip, fin, fout);
            }
        }
        if (cmd) {
            fStack->Execute(std::move(cmd), *fProject);
            // A clip/region edit (move/resize/fade/clip-gain) is baked into the
            // stream at Load, so rebuild the engine to hear it live. Track
            // gain/pan are NOT — UpdateMix applies them every pulse — so skip the
            // rebuild for those (it would add a needless playback dropout).
            const bool liveMix = (fDrag == Drag::Gain || fDrag == Drag::Pan);
            if (!liveMix)
                if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
        }
    }
    const bool wasFader = (fDrag == Drag::Gain || fDrag == Drag::Pan);
    fDrag = Drag::None;
    fDragNote = -1;
    fDragIsMidiClip = false;
    fDragCurLane = -1;
    fMultiMove = false;
    Invalidate();   // a clip may have moved to another lane
    if (wasFader)   // keep the inspector's fader/pan in sync
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
}

void TimelineView::SetRecording(bool active, Frame start, Frame length) {
    fRecording = active;
    fRecStart  = start;
    fRecLen    = length;
    if (!active) {                 // take finished: drop live content
        fLiveNotes.clear();
        fLiveRec = nullptr;
    }
    Invalidate();   // simplest; the region grows every poll anyway
}

void TimelineView::DrawLiveMidi(BRect region, TrackId track) {
    // Map a fixed pitch window across the region height (same span the piano
    // roll centers on). Notes are clip-relative to fRecStart.
    const auto it = fLiveNotes.find(track);
    if (it == fLiveNotes.end()) return;
    const int   loPitch = 36, hiPitch = 96;       // 5 octaves
    const float h = region.Height();
    SetHighColor(Rgb(240, 230, 150));
    for (const MidiNote& n : it->second) {
        float x0 = FrameToX(fRecStart + n.startFrame);
        float x1 = FrameToX(fRecStart + n.startFrame + n.lengthFrames);
        if (x1 < region.left)  continue;
        if (x0 > region.right) continue;
        if (x0 < region.left)  x0 = region.left;
        if (x1 > region.right) x1 = region.right;
        if (x1 < x0 + Themed(1))       x1 = x0 + Themed(1);
        int p = n.pitch;
        if (p < loPitch) p = loPitch;
        if (p > hiPitch) p = hiPitch;
        const float frac = 1.0f - (float)(p - loPitch) / (float)(hiPitch - loPitch);
        const float y = region.top + Themed(2) + frac * (h - Themed(6));
        FillRect(BRect(x0, y, x1, y + Themed(3)));
    }
}

void TimelineView::DrawLiveAudio(BRect region) {
    const size_t count = fLiveRec->EnvCount();
    const float  recRate = fLiveRec->SampleRate();
    if (count == 0 || recRate <= 0.0f) return;
    const double ratio = fLiveProjRate / recRate;         // timeline / source frames
    const double bucketTFrames = (double)Recorder::kEnvBucketFrames * ratio;
    if (bucketTFrames <= 0.0) return;
    // Draw ~one column per pixel: stride buckets by how many fall in a pixel.
    size_t step = (size_t)(fFramesPerPixel / bucketTFrames);
    if (step < 1) step = 1;
    const float mid = (region.top + region.bottom) * 0.5f;
    const float half = region.Height() * 0.5f - Themed(2.0f);
    SetHighColor(ColWave());
    for (size_t i = 0; i < count; i += step) {
        // Peak over the buckets this column spans (so striding loses nothing).
        float lo = 0.0f, hi = 0.0f;
        for (size_t j = i; j < i + step && j < count; j++) {
            const float mn = fLiveRec->EnvMin(j), mx = fLiveRec->EnvMax(j);
            if (mn < lo) lo = mn;
            if (mx > hi) hi = mx;
        }
        const float x = FrameToX(fRecStart + (Frame)((double)i * bucketTFrames));
        if (x < region.left)  continue;
        if (x > region.right) break;
        StrokeLine(BPoint(x, mid - hi * half), BPoint(x, mid - lo * half));
    }
}

void TimelineView::DrawPlayhead() {
    const float x = FrameToX(fPlayhead);
    if (x < HeaderWidth() || x > Bounds().right)
        return;
    SetHighColor(ColPlayhead());
    StrokeLine(BPoint(x, RulerHeight()), BPoint(x, Bounds().bottom));
}

void TimelineView::DrawRuler(BRect update) {
    BRect r = Bounds();
    r.bottom = RulerHeight();

    SetHighColor(ColRuler());
    FillRect(r);

    if (!fProject)
        return;

    // Loop region highlight.
    const Transport& tr = fProject->transport;
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart) {
        float lx0 = FrameToX(tr.loopStart);
        float lx1 = FrameToX(tr.loopEnd);
        if (lx0 < HeaderWidth()) lx0 = HeaderWidth();
        if (lx1 > lx0) {
            SetHighColor(Rgb(70, 110, 90));
            FillRect(BRect(lx0, 0, lx1, RulerHeight()));
        }
    }
    // Punch region (Ctrl-drag): a red band on the lower half of the ruler.
    if (tr.punchEnabled && tr.punchOut > tr.punchIn) {
        float px0 = FrameToX(tr.punchIn);
        float px1 = FrameToX(tr.punchOut);
        if (px0 < HeaderWidth()) px0 = HeaderWidth();
        if (px1 > px0) {
            SetHighColor(Rgb(150, 60, 60));
            FillRect(BRect(px0, RulerHeight() - Themed(6), px1, RulerHeight()));
        }
    }

    // Bar/beat ticks: bars full-height + numbered, beats short (when zoomed in).
    ForEachGridLine([&](float x, bool isBar, long bar) {
        SetHighColor(isBar ? ColText() : ColGrid());
        StrokeLine(BPoint(x, isBar ? 0 : RulerHeight() - Themed(8)),
                   BPoint(x, RulerHeight()));
        if (isBar) {
            char label[16];
            std::snprintf(label, sizeof(label), "%ld", bar);
            DrawString(label, BPoint(x + Themed(3), RulerHeight() - Themed(9)));
        }
    });

    // Tempo/meter change markers (right-click the ruler to add/remove).
    const TempoMap& tm = fProject->tempoMap;
    const auto& tempos = tm.Tempos();
    for (std::size_t i = 0; i < tempos.size(); i++) {
        const TempoChange& t = tempos[i];
        const float x = FrameToX(t.frame);
        const bool  onScreen = (x >= HeaderWidth() && x <= r.right);
        // A ramp draws a diagonal from this marker to the next, sloping up when
        // accelerating; may start off the left edge, so don't gate purely on x.
        if (t.ramp && i + 1 < tempos.size()) {
            const float nx = FrameToX(tempos[i + 1].frame);
            if (nx >= HeaderWidth() && x <= r.right) {
                const float x0 = std::max(x, (float)HeaderWidth());
                const float x1 = std::min(nx, r.right);
                const bool  up = tempos[i + 1].bpm > t.bpm;
                SetHighColor(Rgb(230, 170, 70));
                StrokeLine(BPoint(x0, up ? RulerHeight() - Themed(3) : Themed(3)),
                           BPoint(x1, up ? Themed(3) : RulerHeight() - Themed(3)));
            }
        }
        if (!onScreen) continue;
        SetHighColor(Rgb(230, 170, 70));
        StrokeLine(BPoint(x, 0), BPoint(x, RulerHeight()));
        char s[16];
        std::snprintf(s, sizeof(s), t.ramp ? "%.0f~" : "%.0f", t.bpm);
        DrawString(s, BPoint(x + Themed(2), Themed(9)));
    }
    for (const MeterChange& m : tm.Meters()) {
        const float x = FrameToX(m.frame);
        if (x < HeaderWidth() || x > r.right) continue;
        SetHighColor(Rgb(120, 190, 230));
        char s[16]; std::snprintf(s, sizeof(s), "%d/%d", m.num, m.denom);
        DrawString(s, BPoint(x + Themed(2), Themed(19)));
    }

    // Position markers: a small flag + name at the top of the ruler.
    for (const Marker& mk : fProject->markers) {
        const float x = FrameToX(mk.frame);
        if (x < HeaderWidth() || x > r.right) continue;
        SetHighColor(ColMidiAccent());   // green flag, distinct from tempo/meter
        FillRect(BRect(x, 0, x + Themed(8), Themed(7)));
        StrokeLine(BPoint(x, 0), BPoint(x, RulerHeight()));
        SetHighColor(ColText());
        DrawString(mk.name.c_str(), BPoint(x + Themed(10), Themed(8)));
    }
}

void TimelineView::JumpToMarker(int dir) {
    const Frame ph = fProject->transport.playhead;
    Frame target = -1;
    if (dir > 0) {   // markers are sorted ascending: first after the playhead
        for (const Marker& m : fProject->markers)
            if (m.frame > ph) { target = m.frame; break; }
    } else {         // last before the playhead
        for (const Marker& m : fProject->markers)
            if (m.frame < ph) target = m.frame; else break;
    }
    if (target >= 0) {
        fProject->transport.playhead = target;
        if (BWindow* w = Window()) w->PostMessage(kMsgSeek);
    }
}

void TimelineView::LoopBetweenMarkers() {
    const Frame ph = fProject->transport.playhead;
    Frame lo = -1, hi = -1;
    for (const Marker& m : fProject->markers) {
        if (m.frame <= ph) lo = m.frame;
        if (m.frame > ph)  { hi = m.frame; break; }
    }
    if (lo < 0 || hi < 0 || hi <= lo) return;   // need a bracketing pair
    Transport& tr = fProject->transport;
    tr.loopStart = lo; tr.loopEnd = hi; tr.loopEnabled = true;
    Invalidate();
}

// The marker whose flag is under `where` on the ruler, or nullptr.
const Marker* TimelineView::MarkerAt(BPoint where) const {
    if (!fProject || where.y >= RulerHeight()) return nullptr;
    const Marker* best = nullptr; float bestd = 9.0f;
    for (const Marker& mk : fProject->markers) {
        const float d = std::fabs(FrameToX(mk.frame) - where.x);
        if (d < bestd) { bestd = d; best = &mk; }
    }
    return best;
}

// Walk the visible bar/beat gridlines once, invoking fn for each. Shared by
// the ruler and the lane background so their grids can't drift apart.
void TimelineView::ForEachGridLine(
        const std::function<void(float, bool, long)>& fn) const {
    const TempoMap& tm = fProject->tempoMap;
    const Frame leftFrame  = XToFrame(HeaderWidth());
    const Frame rightFrame = XToFrame(Bounds().right);
    if (rightFrame <= leftFrame) return;

    // Beat spacing varies with tempo; gate beat lines on the local spacing at
    // the left edge (good enough for the visible span).
    const double fpbLeft = tm.FramesPerBeatAt(leftFrame < 0 ? 0 : leftFrame);
    if (fpbLeft < 1.0) return;
    const bool drawBeats = (fpbLeft / fFramesPerPixel) >= 8.0;

    long firstBeat = (long)std::floor(tm.BeatAt(leftFrame < 0 ? 0 : leftFrame));
    if (firstBeat < 0) firstBeat = 0;

    for (long beat = firstBeat; ; beat++) {
        const Frame f = tm.FrameAt((double)beat);
        if (f > rightFrame) break;
        const float x = FrameToX(f);
        if (x < HeaderWidth()) continue;
        int bar = 1, bb = 1;
        tm.BarBeat(f, &bar, &bb);
        const bool isBar = (bb == 1);
        if (!isBar && !drawBeats) continue;
        fn(x, isBar, (long)bar);
    }
}

// Lane height for a track (clamped), falling back to the default.
static float LaneHeightOf(const Track& t) {
    // Floor at the default so the header controls always fit; taller lanes just
    // add waveform room.
    float h = (float)t.height;
    if (h < TrackHeight()) h = TrackHeight();
    if (h > Themed(300.0f)) h = Themed(300.0f);
    return h;
}

float TimelineView::ContentHeight() const {
    float h = 0.0f;
    for (const Track& t : fProject->Tracks())
        h += LaneHeightOf(t) + TrackGap();
    return h;
}

BRect TimelineView::LaneRect(int index) const {
    // Sum the heights of all lanes above `index` (variable per-track heights),
    // offset by the vertical scroll.
    float top = RulerHeight() - fScrollY;
    const auto& tracks = fProject->Tracks();
    for (int i = 0; i < index && i < (int)tracks.size(); i++)
        top += LaneHeightOf(tracks[i]) + TrackGap();
    const float h = (index >= 0 && index < (int)tracks.size())
                    ? LaneHeightOf(tracks[index]) : TrackHeight();
    return BRect(0, top, const_cast<TimelineView*>(this)->Bounds().right,
                 top + h);
}

void TimelineView::DrawLanes(BRect update) {
    if (!fProject)
        return;

    int idx = 0;
    for (const Track& t : fProject->Tracks()) {
        BRect lane = LaneRect(idx);
        // Off-screen lanes cost nothing: the draw is handed the update rect
        // precisely so it can skip them (idx still advances -- it names the
        // lane, not the row on screen).
        if (!lane.Intersects(update)) { idx++; continue; }

        SetHighColor((idx & 1) ? ColLaneAlt() : ColLane());
        FillRect(lane);

        // Bar/beat grid lines through the lane content area (bars brighter).
        // Lines outside the update rect are skipped: on a long project the
        // loop covered every bar in the timeline, most of them invisible.
        ForEachGridLine([&](float x, bool isBar, long) {
            if (x < update.left || x > update.right) return;
            SetHighColor(isBar ? ColGrid() : ColLaneAlt());
            StrokeLine(BPoint(x, lane.top), BPoint(x, lane.bottom));
        });

        // Effective fades for this lane: the clips' own fades folded together
        // with any auto-crossfade implied by overlaps. Same call the engine and
        // the exporter make, so the drawing shows the fades that actually sound.
        const std::vector<ClipFades>& fades = FadesFor(t);

        std::size_t ci = 0;
        for (const Clip& c : t.clips) {
            const ClipFades& ef = fades[ci++];
            if (c.takeGroup > 0 && !c.takeActive)
                continue;   // only the active take of a group is drawn
            DrawClip(c, lane, TrackColor(t.colorIndex), ef.fadeIn, ef.fadeOut,
                     update);
            if (c.takeGroup > 0) {   // "T k/N" badge on the active take
                int n = 0, k = 0;
                for (const Clip& o : t.clips)
                    if (o.takeGroup == c.takeGroup) {
                        n++;
                        if (o.sourceOffset <= c.sourceOffset) k++;
                    }
                float bx = FrameToX(c.startFrame) + Themed(4);
                if (bx < HeaderWidth() + Themed(2)) bx = HeaderWidth() + Themed(2);
                char tb[16];
                std::snprintf(tb, sizeof(tb), "T%d/%d", k, n);
                SetHighColor(Rgb(240, 220, 120));
                DrawString(tb, BPoint(bx, lane.bottom - Themed(16)));
            }
        }
        DrawCrossfades(t, lane);

        if (t.type == TrackType::Midi)
            DrawMidiNotes(t, lane);

        // Live recording region on each armed track (grows each poll).
        if (fRecording && t.armed && fRecLen > 0) {
            float rx0 = FrameToX(fRecStart);
            float rx1 = FrameToX(fRecStart + fRecLen);
            if (rx0 < HeaderWidth()) rx0 = HeaderWidth();
            if (rx1 > lane.right)   rx1 = lane.right;
            if (rx1 > rx0) {
                BRect rb(rx0, lane.top + Themed(3), rx1, lane.bottom - Themed(3));
                SetHighColor(Rgb(150, 50, 50));
                FillRect(rb);
                SetHighColor(ColPlayhead());
                StrokeRect(rb);
                // Live take content: MIDI notes (armed MIDI tracks) or the audio
                // waveform envelope streamed from the recorder, drawn as it grows.
                if (t.type == TrackType::Midi && fLiveNotes.count(t.id))
                    DrawLiveMidi(rb, t.id);
                else if (t.type == TrackType::Audio && fLiveRec)
                    DrawLiveAudio(rb);
                SetHighColor(ColText());
                DrawString("\xE2\x97\x8F REC", BPoint(rb.left + Themed(4), rb.top + Themed(14)));
            }
        }

        // Automation curve overlay when this track's Auto mode is on.
        if (auto am = fAutoMode.find(t.id);
            am != fAutoMode.end() && am->second != 0)
            DrawAutomation(t, lane, am->second);

        DrawTrackHeader(t, lane);   // header gutter on top of the lane

        idx++;
    }
}

// The left gutter for one track: name, mute/solo toggles, gain fader, pan bar.
// Custom-drawn (not BControls) so it stays pixel-aligned with the lane and
// needs no per-track child-view bookkeeping.
void TimelineView::DrawTrackHeader(const Track& t, BRect lane) {
    BRect hdr(0, lane.top, HeaderWidth(), lane.bottom);
    const bool selected = (t.id == fSelectedTrack);
    SetHighColor(selected ? ColHeaderHi() : ColHeader());
    FillRect(hdr);
    // Left color strip by track type: audio = blue, MIDI = green, bus = grey.
    SetHighColor(t.type == TrackType::Midi ? ColMidiAccent()
               : t.type == TrackType::Bus  ? ColTextDim() : ColAudioAccent());
    FillRect(BRect(0, lane.top, Themed(4), lane.bottom));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(HeaderWidth() - 1, lane.top),
               BPoint(HeaderWidth() - 1, lane.bottom));
    // Selection: subtle blue outline (not a harsh solid block).
    if (t.id == fSelectedTrack) {
        SetHighColor(ColAccent());
        StrokeRect(BRect(Themed(1), lane.top + Themed(1), HeaderWidth() - Themed(2), lane.bottom - Themed(1)));
    }

    SetHighColor(ColText());
    DrawString(t.name.c_str(), BPoint(Themed(10), lane.top + Themed(14)));

    // Mute / Solo / Record / Input-monitor: rounded state buttons.
    DrawButton(this, MuteRect(lane), "M", t.muted,        ColMute());
    DrawButton(this, SoloRect(lane), "S", t.soloed,       ColSolo());
    DrawButton(this, ArmRect(lane),  "R", t.armed,        ColRec());
    DrawButton(this, MonRect(lane),  "I", t.inputMonitor, ColMon());
    if (t.soloSafe) {   // solo-safe: a small dot on the Solo button
        BRect s = SoloRect(lane);
        SetHighColor(ColSolo());
        FillEllipse(BPoint(s.right - Themed(3), s.top + Themed(3)), Themed(2), Themed(2));
    }

    // Pan knob (value ring in the track-type accent).
    DrawKnob(this, PanKnobRect(lane), t.pan,
             t.type == TrackType::Midi ? ColMidiAccent() : ColAudioAccent());

    // Gain fader (horizontal): recessed trough + accent fill.
    BRect g = GainRect(lane);
    SetHighColor(ColGrid());  FillRect(g);
    float gf = t.gain / kMaxGain; if (gf < 0) gf = 0; if (gf > 1) gf = 1;
    BRect gfill = g; gfill.right = g.left + (g.Width()) * gf;
    SetHighColor(ColAccent());  FillRect(gfill);
    SetHighColor(ColBtnBorder());  StrokeRect(g);

    // Per-track stereo meter at the header's right edge.
    const float mx0 = HeaderWidth() - HdrMeterW();
    BRect meterBox(mx0, lane.top + Themed(2), HeaderWidth() - Themed(2), lane.bottom - Themed(2));
    SetHighColor(Rgb(16, 16, 20));
    FillRect(meterBox);
    float mPeakL = 0.0f, mPeakR = 0.0f;
    if (auto it = fTrackPeaks.find(t.id); it != fTrackPeaks.end()) {
        mPeakL = it->second.first; mPeakR = it->second.second;
    }
    const float bw = (meterBox.Width() - Themed(3)) * 0.5f;
    auto meterBar = [&](float x0, float level) {
        if (level < 0.0f) level = 0.0f; if (level > 1.0f) level = 1.0f;
        const float h = meterBox.Height() * level;
        BRect b(x0, meterBox.bottom - h, x0 + bw, meterBox.bottom);
        SetHighColor(MeterColor(level));
        FillRect(b);
    };
    meterBar(meterBox.left + Themed(1), mPeakL);
    meterBar(meterBox.left + Themed(2) + bw, mPeakR);
}

// --- Automation editing --------------------------------------------------

// Resolve a track's automation mode to a lane + value range + static default.
bool TimelineView::AutoRefFor(const Track& t, int mode, AutoRef* out) const {
    if (mode == 1) { *out = { &t.gainAuto, 0.0f, kMaxGain, t.gain, -1 }; return true; }
    if (mode == 2) { *out = { &t.panAuto, -1.0f, 1.0f, t.pan, -1 }; return true; }
    const int fi = mode - 3;
    if (fi < 0 || fi >= (int)t.fxAuto.size()) return false;
    const FxAutoLane& fa = t.fxAuto[(size_t)fi];
    EffectType et = (fa.fxIndex >= 0 && fa.fxIndex < (int)t.fx.size())
                    ? t.fx[(size_t)fa.fxIndex].type : EffectType::Eq;
    float mn = 0, mx = 1;
    FxParamRange(et, fa.slot, &mn, &mx);
    const float def = (fa.fxIndex >= 0 && fa.fxIndex < (int)t.fx.size())
                      ? t.fx[(size_t)fa.fxIndex].p((size_t)fa.slot) : 0.0f;
    *out = { &fa.lane, mn, mx, def, fi };
    return true;
}

float TimelineView::AutoValueToY(BRect lane, float mn, float mx, float v) const {
    const float top = lane.top + Themed(4), bot = lane.bottom - Themed(4);
    float t = (mx > mn) ? (v - mn) / (mx - mn) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    return bot - t * (bot - top);
}

float TimelineView::AutoYToValue(BRect lane, float mn, float mx, float y) const {
    const float top = lane.top + Themed(4), bot = lane.bottom - Themed(4);
    float t = (bot - y) / (bot - top);
    if (t < 0) t = 0; if (t > 1) t = 1;
    return mn + t * (mx - mn);
}

int TimelineView::AutoPointAt(const AutomationLane& al, BRect lane, float mn,
                              float mx, BPoint where) const {
    for (size_t i = 0; i < al.Count(); i++) {
        const float x = FrameToX(al.At(i).frame);
        const float y = AutoValueToY(lane, mn, mx, al.At(i).value);
        if (std::fabs(x - where.x) <= Themed(5.0f) && std::fabs(y - where.y) <= Themed(5.0f))
            return (int)i;
    }
    return -1;
}

void TimelineView::DrawAutomation(const Track& t, BRect lane, int mode) {
    AutoRef ref;
    if (!AutoRefFor(t, mode, &ref)) return;
    const AutomationLane& al = *ref.lane;
    const float x0 = HeaderWidth(), x1 = lane.right;

    SetHighColor(Rgb(230, 200, 90));
    float px = x0, py = AutoValueToY(lane, ref.mn, ref.mx,
                                     al.ValueAt(XToFrame(x0), ref.def));
    for (float x = x0 + 2.0f; x <= x1; x += 2.0f) {
        const float y = AutoValueToY(lane, ref.mn, ref.mx,
                                     al.ValueAt(XToFrame(x), ref.def));
        StrokeLine(BPoint(px, py), BPoint(x, y));
        px = x; py = y;
    }
    SetHighColor(Rgb(255, 232, 120));
    for (size_t i = 0; i < al.Count(); i++) {
        const float x = FrameToX(al.At(i).frame);
        if (x < x0 || x > x1) continue;
        const float y = AutoValueToY(lane, ref.mn, ref.mx, al.At(i).value);
        FillRect(BRect(x - Themed(3), y - Themed(3), x + Themed(3), y + Themed(3)));
    }
}

// Commit an edited automation lane (gain/pan or an fx-param lane) as one undo
// step. `fxIndex` < 0 selects gain/pan by `mode`.
void TimelineView::CommitAuto(TrackId track, int mode, int fxIndex,
                              const AutomationLane& lane) {
    if (fxIndex >= 0)
        fStack->Execute(std::make_unique<SetFxAutoLaneCommand>(track, fxIndex, lane),
                        *fProject);
    else
        fStack->Execute(std::make_unique<SetAutoLaneCommand>(track,
            mode == 2 ? AutoLaneKind::Pan : AutoLaneKind::Gain, lane), *fProject);
}

void TimelineView::HandleAutoMouseDown(const Track& t, BRect lane, int idx,
                                       BPoint where, bool rightClick) {
    const int mode = fAutoMode[t.id];
    AutoRef ref;
    if (!AutoRefFor(t, mode, &ref)) return;
    const AutomationLane& al = *ref.lane;
    int hit = AutoPointAt(al, lane, ref.mn, ref.mx, where);

    if (rightClick) {   // delete a handle
        if (hit >= 0) {
            AutomationLane nl = al;
            nl.RemovePoint((size_t)hit);
            CommitAuto(t.id, mode, ref.fxIndex, nl);
            Invalidate(lane);
        }
        return;
    }

    // Snapshot the lane for undo, then begin a live-edit gesture on the model.
    fAutoOrig = al;
    Track* tm = fProject->FindTrack(t.id);
    if (!tm) return;
    AutomationLane* live = (ref.fxIndex >= 0)
        ? &tm->fxAuto[(size_t)ref.fxIndex].lane
        : (mode == 2 ? &tm->panAuto : &tm->gainAuto);

    if (hit < 0) {   // empty: add a point
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        live->AddPoint(f, AutoYToValue(lane, ref.mn, ref.mx, where.y));
        fAutoDragFrame = f;
    } else {
        fAutoDragFrame = al.At((size_t)hit).frame;
    }

    fAutoDragging = true;
    fAutoTrack    = t.id;
    fAutoFx       = ref.fxIndex;
    fAutoMn = ref.mn; fAutoMx = ref.mx;
    fDragLane     = idx;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate(lane);
}

// Where two clips overlap, mark the auto-crossfade: a tinted band over the
// overlap plus the crossing ramps (earlier clip out, later clip in). Drawn after
// the lane's blocks, because the later clip is painted on top of the earlier one
// and would otherwise hide its fade-out. The overlap comes from the same
// CrossfadeOverlap the renderers use, so this can't disagree with what sounds.
void TimelineView::DrawCrossfades(const Track& t, BRect lane) {
    if (t.clips.size() < 2) return;
    const float top = lane.top + Themed(3), bot = lane.bottom - Themed(3);

    for (std::size_t i = 0; i + 1 < t.clips.size(); i++) {
        const Clip& a = t.clips[i];
        const Clip& b = t.clips[i + 1];
        const Frame ov = CrossfadeOverlap(a, b);
        if (ov <= 0) continue;

        float x0 = FrameToX(b.startFrame);
        float x1 = FrameToX(b.startFrame + ov);
        if (x1 < HeaderWidth() || x0 > lane.right) continue;
        if (x0 < HeaderWidth()) x0 = HeaderWidth();
        if (x1 > lane.right)   x1 = lane.right;
        if (x1 <= x0) continue;

        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        SetHighColor(255, 232, 120, 40);            // warm tint over the overlap
        FillRect(BRect(x0, top, x1, bot));
        SetDrawingMode(B_OP_COPY);

        SetHighColor(Rgb(255, 232, 120));
        StrokeLine(BPoint(x0, top), BPoint(x1, bot));   // a fading out
        StrokeLine(BPoint(x0, bot), BPoint(x1, top));   // b fading in
        SetHighColor(ColClipBorder());
        StrokeLine(BPoint(x0, top), BPoint(x0, bot));   // overlap boundaries
        StrokeLine(BPoint(x1, top), BPoint(x1, bot));
    }
}

void TimelineView::DrawClip(const Clip& c, BRect lane, rgb_color base,
                            Frame fadeIn, Frame fadeOut, BRect update) {
    float x0 = FrameToX(c.startFrame);
    float x1 = FrameToX(c.startFrame + c.lengthFrames);
    if (x1 < HeaderWidth() || x0 > lane.right)
        return;                       // fully outside the content area
    // ...and nothing of it is being repainted.
    if (x1 < update.left || x0 > update.right)
        return;
    if (x0 < HeaderWidth()) x0 = HeaderWidth();

    BRect block(x0, lane.top + Themed(3), x1, lane.bottom - Themed(3));
    SetHighColor(base);
    FillRoundRect(block, Themed(5), Themed(5));        // Logic-style rounded region

    DrawClipWave(c, block, update);

    // Fade ramps: a diagonal from the block corner up to where the fade ends.
    // These are the effective fades, so an auto-crossfade with a neighbour draws
    // here too rather than only being audible.
    SetHighColor(ColClipBorder());
    if (fadeIn > 0) {
        float fx = FrameToX(c.startFrame + fadeIn);
        if (fx > block.left)
            StrokeLine(BPoint(block.left, block.bottom),
                       BPoint(fx < block.right ? fx : block.right, block.top));
    }
    if (fadeOut > 0) {
        float fx = FrameToX(c.startFrame + c.lengthFrames - fadeOut);
        if (fx < block.right)
            StrokeLine(BPoint(fx > block.left ? fx : block.left, block.top),
                       BPoint(block.right, block.bottom));
    }

    // Per-clip gain: a horizontal line across the block at the gain level
    // (top = kMaxGain, bottom = 0), plus a dB label when not at unity.
    if (block.Width() > Themed(24)) {
        float gf = c.gain / kMaxGain; if (gf < 0) gf = 0; if (gf > 1) gf = 1;
        const float gy = block.bottom - gf * block.Height();
        SetHighColor(Rgb(255, 232, 120));
        StrokeLine(BPoint(block.left, gy), BPoint(block.right, gy));
        if (std::fabs(c.gain - 1.0f) > 0.01f) {
            char db[16];
            const float dB = c.gain > 0.0001f ? 20.0f * std::log10(c.gain)
                                              : -99.0f;
            std::snprintf(db, sizeof(db), "%+.1f dB", dB);
            DrawString(db, BPoint(block.left + Themed(4), block.bottom - Themed(4)));
        }
    }

    // Name strip across the top (darker shade of the track color), like Logic.
    const float stripH = Themed(15.0f);
    if (block.Height() > stripH + Themed(2) && block.Width() > Themed(10)) {
        BRect strip(block.left + Themed(1), block.top + Themed(1), block.right - Themed(1),
                    block.top + stripH);
        SetHighColor(Rgb((uint8)(base.red * 0.5f), (uint8)(base.green * 0.5f),
                         (uint8)(base.blue * 0.5f)));
        FillRect(strip);
        const std::string& p = c.sourcePath;
        size_t slash = p.find_last_of('/');
        // Truncated to the strip: a name runs into the next region otherwise.
        BString name((slash == std::string::npos) ? p.c_str()
                                                  : p.c_str() + slash + 1);
        const float room = strip.Width() - Themed(14);
        if (room > Themed(8)) {
            TruncateString(&name, B_TRUNCATE_END, room);
            SetHighColor(Rgb(245, 246, 248));
            DrawString(name.String(), BPoint(strip.left + Themed(12),
                                             strip.top + Themed(11)));
        }
    }

    // Fade grips: small triangles at the top corners (drag left corner for
    // fade-in, right for fade-out). Always shown on wide clips so the gesture
    // is discoverable; the diagonal ramps above show the current fade.
    if (block.Width() > Themed(30)) {
        SetHighColor(Rgb(232, 238, 248));
        BPoint li[3] = { BPoint(block.left + Themed(1), block.top + Themed(1)),
                         BPoint(block.left + Themed(8), block.top + Themed(1)),
                         BPoint(block.left + Themed(1), block.top + Themed(8)) };
        FillPolygon(li, 3);
        BPoint ri[3] = { BPoint(block.right - Themed(1), block.top + Themed(1)),
                         BPoint(block.right - Themed(8), block.top + Themed(1)),
                         BPoint(block.right - Themed(1), block.top + Themed(8)) };
        FillPolygon(ri, 3);
    }

    // Border / selection highlight (rounded).
    if (ClipSelected(c.id)) {
        SetHighColor(Rgb(255, 255, 255));
        StrokeRoundRect(block, Themed(5), Themed(5));
        StrokeRoundRect(block.InsetByCopy(Themed(1), Themed(1)), Themed(4), Themed(4));
    } else {
        SetHighColor(Rgb((uint8)(base.red * 0.7f), (uint8)(base.green * 0.7f),
                         (uint8)(base.blue * 0.7f)));
        StrokeRoundRect(block, Themed(5), Themed(5));
    }
}

// Draw a Midi track's regions: each MidiClip is a block (like an audio clip)
// with a mini piano-roll preview of its in-window notes. Notes are edited in
// the piano roll (double-click a region); the timeline moves/resizes regions.
void TimelineView::DrawMidiNotes(const Track& t, BRect lane) {
    for (const MidiClip& mc : t.midiClips) {
        if (mc.takeGroup > 0 && !mc.takeActive)
            continue;   // only the active take of a group is drawn
        float x0 = FrameToX(mc.startFrame);
        float x1 = FrameToX(mc.startFrame + mc.lengthFrames);
        if (x1 < HeaderWidth() || x0 > lane.right)
            continue;
        BRect block(std::max(x0, (float)HeaderWidth()), lane.top + Themed(3),
                    std::min(x1, lane.right),         lane.bottom - Themed(3));

        rgb_color base = TrackColor(mc.colorIndex ? mc.colorIndex : t.colorIndex);
        SetHighColor(Rgb((uint8)(base.red * 0.55f), (uint8)(base.green * 0.55f),
                         (uint8)(base.blue * 0.55f)));
        FillRoundRect(block, Themed(5), Themed(5));
        // Name strip across the top.
        if (block.Height() > Themed(17) && block.Width() > Themed(10)) {
            BRect strip(block.left + Themed(1), block.top + Themed(1), block.right - Themed(1),
                        block.top + Themed(15));
            SetHighColor(Rgb((uint8)(base.red * 0.32f), (uint8)(base.green * 0.32f),
                             (uint8)(base.blue * 0.32f)));
            FillRect(strip);
            BString name(t.name.c_str());
            const float room = strip.Width() - Themed(6);
            if (room > Themed(8)) {
                TruncateString(&name, B_TRUNCATE_END, room);
                SetHighColor(Rgb(240, 242, 245));
                DrawString(name.String(), BPoint(strip.left + Themed(4),
                                                 strip.top + Themed(11)));
            }
        }
        // "T k/N" take badge on the active region of a loop-record group.
        if (mc.takeGroup > 0 && block.Width() > Themed(26)) {
            int n = 0, k = 0;
            for (const MidiClip& o : t.midiClips)
                if (o.takeGroup == mc.takeGroup) {
                    n++;
                    if (o.id <= mc.id) k++;   // stacked in id (record) order
                }
            char tb[16];
            std::snprintf(tb, sizeof(tb), "T%d/%d", k, n);
            SetHighColor(Rgb(240, 220, 120));
            DrawString(tb, BPoint(block.left + Themed(4), block.bottom - Themed(5)));
        }

        // In-window notes as a light preview below the name strip.
        const float noteTop = block.top + Themed(16);
        const float noteH   = block.bottom - noteTop;
        for (const MidiNote& n : mc.notes) {
            if (n.startFrame < 0 || n.startFrame >= mc.lengthFrames || noteH < Themed(4))
                continue;
            const Frame a = mc.startFrame + n.startFrame;
            float nx0 = FrameToX(a);
            float nx1 = FrameToX(a + n.lengthFrames);
            if (nx1 < block.left || nx0 > block.right)
                continue;
            if (nx0 < block.left)  nx0 = block.left;
            if (nx1 > block.right) nx1 = block.right;
            int p = n.pitch - kMidiLow;
            if (p < 0) p = 0;
            if (p >= kMidiRange) p = kMidiRange - 1;
            const float ny = block.bottom - (float)p / kMidiRange * noteH;
            const float nh = noteH / kMidiRange + Themed(1.0f);
            const float s = 0.55f + 0.45f * (n.velocity / 127.0f);
            SetHighColor(Rgb((uint8)(235 * s), (uint8)(240 * s), (uint8)(245 * s)));
            FillRect(BRect(nx0, ny - nh, nx1, ny));
        }

        // Velocity-fade ramps + top-corner grips (drag to set MIDI fades).
        SetHighColor(Rgb(235, 240, 248));
        if (mc.fadeInFrames > 0) {
            float fx = FrameToX(mc.startFrame + mc.fadeInFrames);
            if (fx > block.left)
                StrokeLine(BPoint(block.left, block.bottom),
                           BPoint(std::min(fx, block.right), block.top));
        }
        if (mc.fadeOutFrames > 0) {
            float fx = FrameToX(mc.startFrame + mc.lengthFrames - mc.fadeOutFrames);
            if (fx < block.right)
                StrokeLine(BPoint(std::max(fx, block.left), block.top),
                           BPoint(block.right, block.bottom));
        }
        if (block.Width() > Themed(30)) {
            BPoint li[3] = { BPoint(block.left + Themed(1), block.top + Themed(1)),
                             BPoint(block.left + Themed(8), block.top + Themed(1)),
                             BPoint(block.left + Themed(1), block.top + Themed(8)) };
            FillPolygon(li, 3);
            BPoint ri[3] = { BPoint(block.right - Themed(1), block.top + Themed(1)),
                             BPoint(block.right - Themed(8), block.top + Themed(1)),
                             BPoint(block.right - Themed(1), block.top + Themed(8)) };
            FillPolygon(ri, 3);
        }

        if (ClipSelected(mc.id)) {           // selection highlight (shared)
            SetHighColor(Rgb(255, 255, 255));
            StrokeRoundRect(block, Themed(5), Themed(5));
            StrokeRoundRect(block.InsetByCopy(Themed(1), Themed(1)), Themed(4), Themed(4));
        } else {
            SetHighColor(Rgb((uint8)(base.red * 0.75f), (uint8)(base.green * 0.75f),
                             (uint8)(base.blue * 0.75f)));
            StrokeRoundRect(block, Themed(5), Themed(5));
        }
    }
}

// Paint the min/max envelope inside a clip block: one vertical line per pixel
// column, from the column's min sample to its max. Reads a handful of peak
// buckets per column (never scans the audio). No cache for this clip's source
// -> just the flat filled block.
void TimelineView::DrawClipWave(const Clip& c, BRect block, BRect update) {
    if (!fPeaks)
        return;
    auto it = fPeaks->find(c.sourcePath);
    if (it == fPeaks->end() || !it->second.IsValid())
        return;
    const PeakCache& pc = it->second;

    const float mid  = (block.top + block.bottom) * 0.5f;
    const float half = (block.bottom - block.top) * 0.5f - Themed(1.0f);

    // Timeline frames are at the project rate; the envelope indexes source
    // frames. Scale by source/project rate so the waveform tracks the audio
    // regardless of the file's sample rate.
    const double projRate = fProject->sampleRate;
    const double srcRate  = pc.SampleRate();
    const double toSrc = (srcRate > 0 && projRate > 0) ? srcRate / projRate : 1.0;

    // Haiku's line array carries the colour per line (SetHighColor does not
    // apply inside BeginLineArray/EndLineArray).
    const rgb_color wave = ColWave();
    // Only the columns being repainted: a clip can span the whole timeline,
    // and the loop used to walk all of it (one StrokeLine per column) even
    // when a sliver was visible.
    float lo = block.left  > update.left  ? block.left  : update.left;
    float hi = block.right < update.right ? block.right : update.right;
    if (lo < HeaderWidth()) lo = HeaderWidth();
    const int xL = static_cast<int>(lo);
    const int xR = static_cast<int>(hi);
    // Batch the columns: one BeginLineArray per chunk, not one app_server call
    // per column.
    const int kChunk = 256;
    BPoint pts[kChunk * 2];
    int n = 0;
    for (int x = xL; x <= xR; x++) {
        // Timeline frames this column spans -> source frames within the clip.
        const Frame tf0 = XToFrame(static_cast<float>(x));
        const Frame tf1 = XToFrame(static_cast<float>(x + 1));
        const Frame s0 = c.sourceOffset + (Frame)((tf0 - c.startFrame) * toSrc);
        const Frame s1 = c.sourceOffset + (Frame)((tf1 - c.startFrame) * toSrc);
        if (s1 <= 0)
            continue;

        Peak pk = pc.Range(s0 < 0 ? 0 : s0, s1);
        const float yMax = mid - pk.max * half;   // max amplitude -> up
        const float yMin = mid - pk.min * half;   // min amplitude -> down
        pts[n++] = BPoint(x, yMin);
        pts[n++] = BPoint(x, yMax);
        if (n == kChunk * 2) {
            BeginLineArray(n / 2);
            for (int i = 0; i < n; i += 2)
                AddLine(pts[i], pts[i + 1], wave);
            EndLineArray();
            n = 0;
        }
    }
    if (n > 0) {
        BeginLineArray(n / 2);
        for (int i = 0; i < n; i += 2)
            AddLine(pts[i], pts[i + 1], wave);
        EndLineArray();
    }
}

} // namespace daw
