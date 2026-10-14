#include "TimelineView.h"

#include "UiMetrics.h"
#include "EffectsWindow.h"
#include "SendsWindow.h"
#include "InstrumentWindow.h"
#include "PianoRoll.h"
#include "SampleBrowser.h"   // kMsgSampleDrag / kMsgBrowserImport
#include "RenameWindow.h"
#include "MainWindow.h"      // MSG_NEW_AUDIO / MSG_NEW_MIDI (the lane menu's track entries)
#include "../engine/AudioFormats.h"  // SniffAudioFileFormat (the drop filter)
#include "TrackInputs.h"     // the ONE input-choice list (header + inspector)
#include "widgets/DawIcons.h"   // the shared glyphs (arrange tools, slip)
#include "../engine/Recorder.h" // live capture waveform envelope
#include "../model/Crossfade.h" // effective (auto-crossfade) clip fades
#include "../model/RangeEdit.h" // ExtractRange (the M2.4 range operations)
#include "../model/ClipStretch.h" // the stretch drag's ratio policy (M7.2)
#include "Widgets.h"            // shared pan knob draw

#include <Bitmap.h>
#include <Cursor.h>
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

// --- the scrollbars -------------------------------------------------------
// A BScrollBar with no target view only updates itself and calls
// ValueChanged() -- which is exactly the hook a view that scrolls by offset
// needs (the default behaviour, ScrollBy on a target, moves bounds instead).
TimelineScrollBar::TimelineScrollBar(const char* name, orientation dir,
                                     float min, float max,
                                     std::function<void(float)> onValue)
    : BScrollBar(name, nullptr, min, max, dir), fOnValue(std::move(onValue)) {}

void TimelineScrollBar::ValueChanged(float newValue) {
    BScrollBar::ValueChanged(newValue);
    if (fOnValue) fOnValue(newValue);
}

TimelineView::TimelineView(BRect frame, Project* project, CommandStack* stack)
    : BView(frame, "timeline", B_FOLLOW_ALL_SIDES,
            B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE
            | B_SUPPORTS_LAYOUT),
      fProject(project),
      fStack(stack),
      fFramesPerPixel(kDefaultFramesPerPixel),
      fScrollFrame(0) {
    ApplyTheme();
    // Real scrollbars (M2.2). Children rather than a BScrollView: the view
    // scrolls by frame offset, not by moving its bounds, so the scrollbars must
    // drive it rather than scroll it. A B_SUPPORTS_LAYOUT view does not
    // reposition children by follow modes (BView::_ResizeBy), so FrameResized
    // places them.
    fHBar = new TimelineScrollBar("tlhscroll", B_HORIZONTAL, 0.0f, 1.0f,
                                  [this](float v) { ScrollToFrame((Frame)v); });
    fVBar = new TimelineScrollBar("tlvscroll", B_VERTICAL, 0.0f, 1.0f,
                                  [this](float v) { ScrollToY(v); });
    for (BScrollBar* b : { (BScrollBar*)fHBar, (BScrollBar*)fVBar }) {
        float w = 0.0f, h = 0.0f;
        b->GetPreferredSize(&w, &h);
        fBarThickness = std::max(b->Orientation() == B_HORIZONTAL ? h : w,
                                 Themed(10.0f));
        // The vertical bar's range is in PIXELS, so the stock steps are the
        // right size for it. The horizontal bar's range is in FRAMES, so its
        // steps have to be frames too and follow the zoom: SyncScrollBars sets
        // them through ScrollStepsFor (package 50 — the stock 12/120 moved the
        // view by a fiftieth of a pixel).
        if (b->Orientation() == B_VERTICAL) b->SetSteps(12.0f, 120.0f);
        AddChild(b);
    }
    LayoutScrollBars();
}

// A mode switch: the lane colour is cached, everything else this view draws is
// read from the tokens at draw time.
void TimelineView::ApplyTheme() {
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

// Edits snap to the grid the snap menu selects (M2.3, model/SnapGrid.h). That
// header's default keeps the editor's historical 16th-note behaviour.

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
    return BRect(Themed(6),   lane.top + Themed(60),
                 Themed(128), lane.top + Themed(72));
}

// --- track depth (M2.6): the header's new controls ------------------------

// The fold triangle, the colour swatch and the input field. HeaderControlAt,
// the drawing and HeaderRectFor all come through these, so a control cannot be
// drawn somewhere and clicked somewhere else.
static BRect FoldRect(BRect lane) {
    return BRect(Themed(5),  lane.top + Themed(3),
                 Themed(15), lane.top + Themed(15));
}
static BRect ColourSwatchRect(BRect lane) {
    return BRect(Themed(116), lane.top + Themed(2),
                 Themed(132), lane.top + Themed(16));
}
static BRect InputRect(BRect lane) {
    return BRect(Themed(6),  lane.top + Themed(44),
                 Themed(96), lane.top + Themed(58));
}
// The name field: what the in-place rename edits and where it is drawn.
static BRect NameRect(BRect lane) {
    return BRect(Themed(18), lane.top + Themed(1),
                 Themed(112), lane.top + Themed(17));
}
// The collapsed lane's compact row (M2.6): triangle, swatch, name, M, S. The
// meter stays on the right edge.
static BRect FoldRectCompact(BRect lane) {
    return BRect(Themed(5),  lane.top + Themed(3),
                 Themed(15), lane.top + Themed(17));
}
static BRect ColourSwatchRectCompact(BRect lane) {
    return BRect(Themed(20), lane.top + Themed(4),
                 Themed(34), lane.top + Themed(18));
}
static BRect MuteRectCompact(BRect lane) {
    return BRect(Themed(96),  lane.top + Themed(3),
                 Themed(112), lane.top + Themed(19));
}
static BRect SoloRectCompact(BRect lane) {
    return BRect(Themed(116), lane.top + Themed(3),
                 Themed(132), lane.top + Themed(19));
}

float TimelineView::FrameToX(Frame f) const {
    return HeaderWidth()
         + static_cast<float>((f - fScrollFrame) / fFramesPerPixel);
}

Frame TimelineView::XToFrame(float x) const {
    return fScrollFrame
         + static_cast<Frame>((x - HeaderWidth()) * fFramesPerPixel);
}

void TimelineView::ZoomBy(double factor) {
    // The keyboard's zoom (and the menu's) anchors on the playhead, so the bar
    // the user is working on stays where it is. An off-screen playhead would
    // drag the view with it, so the view's centre is the fallback.
    float anchorX = Bounds().right;
    const float px = FrameToX(fPlayhead);
    if (px >= HeaderWidth() && px <= Bounds().right)
        anchorX = px;
    else
        anchorX = (HeaderWidth() + Bounds().right) * 0.5f;
    ZoomAnchoredAt(factor, anchorX);
}

void TimelineView::ZoomAnchoredAt(double factor, float x) {
    const Frame anchor  = XToFrame(x);
    const Frame wasScroll = fScrollFrame;
    double fpp = fFramesPerPixel * factor;
    if (fpp < 16.0)    fpp = 16.0;      // most zoomed-in
    if (fpp > 65536.0) fpp = 65536.0;   // most zoomed-out
    fFramesPerPixel = fpp;
    // Put the anchor frame back under x: the same pixel maps to it again.
    fScrollFrame = anchor - (Frame)((x - HeaderWidth()) * fFramesPerPixel);
    if (fScrollFrame < 0) fScrollFrame = 0;
    // Never pull the view backwards: zooming into empty space past the content
    // is legitimate (the user pointed at it), but a zoom must not scroll a
    // view back to the content on its own.
    const Frame ceiling = std::max(ContentEndFrame(), wasScroll);
    if (fScrollFrame > ceiling) fScrollFrame = ceiling;
    Invalidate();
}

// Where the project's content ends: the last clip/region, the playhead, the
// loop and punch ranges, and the markers. The horizontal scroll goes this far
// (M2.2 "you can scroll past the end of the last clip"), so a project whose
// last clip is short can still be scrolled off the left edge.
Frame TimelineView::ContentEndFrame() const {
    Frame end = 0;
    auto bump = [&end](Frame f) { if (f > end) end = f; };
    if (fProject) {
        for (const Track& t : fProject->Tracks()) {
            for (const Clip& c : t.clips)
                bump(c.startFrame + c.lengthFrames);
            for (const MidiClip& mc : t.midiClips)
                bump(mc.startFrame + mc.lengthFrames);
        }
        const Transport& tr = fProject->transport;
        bump(tr.playhead);
        if (tr.loopEnabled)  bump(tr.loopEnd);
        if (tr.punchEnabled) bump(tr.punchOut);
        for (const Marker& m : fProject->markers) bump(m.frame);
    }
    return end;
}

float TimelineView::VisibleFrames() const {
    const float w = Bounds().Width() - HeaderWidth() - fBarThickness;
    if (w < 1.0f) return 0.0f;
    return (float)(w * fFramesPerPixel);
}

void TimelineView::PanBy(Frame deltaFrames) {
    ScrollToFrame(fScrollFrame + deltaFrames);
}

void TimelineView::ScrollToFrame(Frame f) {
    if (f < 0) f = 0;
    // Past the end of the last clip, up to the content end: the whole
    // arrangement can be scrolled off the left edge (M2.2).
    const Frame end = ContentEndFrame();
    if (f > end) f = end;
    if (f == fScrollFrame) return;
    fScrollFrame = f;
    Invalidate();
}

void TimelineView::ScrollVerticalBy(float dy) {
    ScrollToY(fScrollY + dy);
}

void TimelineView::ScrollToY(float y) {
    // The visible height is what the lanes get: the horizontal scrollbar sits
    // over the view's bottom strip, so a scroll that ignored it left the last
    // lane's bottom row UNDER the bar (the collapsed-lane shot showed it).
    const float viewH = Bounds().Height() - ContentTop() - fBarThickness;
    const float maxScroll = ContentHeight() - viewH;
    if (y > maxScroll) y = maxScroll;
    if (y < 0.0f)      y = 0.0f;   // (also clamps when all fits)
    if (y == fScrollY) return;
    fScrollY = y;
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

// Clear one lane meter's clip lamps (package 52). The lamps latch at digital
// full scale, exactly as the mixer strips' and the master meter's do.
void TimelineView::ClearClipLamps(TrackId id) {
    TrackMeterState& ms = fTrackMeter[id];
    ms.clipL.Reset();
    ms.clipR.Reset();
    Invalidate(BRect(HeaderWidth() - HdrMeterW(), 0, HeaderWidth(),
                     Bounds().bottom));
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
    // RenameWindow's answer for a clip (M2.5): the clip id rides in its
    // echoed "aux" field, the track in "track".
    if (msg->what == kMsgRenameClip) {
        int64 tid = 0, cid = 0;
        const char* name = nullptr;
        msg->FindInt64("track", &tid);
        msg->FindInt64("aux", &cid);
        if (msg->FindString("name", &name) == B_OK && name) {
            if (fStack->Execute(std::make_unique<SetClipNameCommand>(
                                    (TrackId)tid, (ClipId)cid, name), *fProject)) {
                Invalidate();
                if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
            }
        }
        return;
    }
    if (msg->what == B_MOUSE_WHEEL_CHANGED) {
        // Navigation (M2.2): Ctrl zooms anchored on the pointer, Shift (or a
        // horizontal wheel) scrolls sideways, a plain wheel scrolls the tracks.
        float dx = 0.0f, dy = 0.0f;
        msg->FindFloat("be:wheel_delta_x", &dx);
        msg->FindFloat("be:wheel_delta_y", &dy);
        const uint32 mods = EventModifiers(msg);
        // A wheel message carries no pointer position, so the anchor is the
        // last hover point (a synthetic wheel from a test moves it first).
        BPoint where = fHoverPos;
        if (!fHoverValid) where = BPoint(Bounds().right, ContentTop());
        if (mods & B_CONTROL_KEY) {
            const float delta = dy != 0.0f ? dy : dx;
            if (delta != 0.0f) {
                float x = where.x;
                if (x < HeaderWidth()) x = HeaderWidth();
                if (x > Bounds().right) x = Bounds().right;
                ZoomAnchoredAt(delta > 0.0f ? 1.25 : 1.0 / 1.25, x);
                return;
            }
        } else if ((mods & B_SHIFT_KEY) != 0 || (dy == 0.0f && dx != 0.0f)) {
            const float delta = dy != 0.0f ? dy : dx;
            PanBy((Frame)(delta * 8.0f * fFramesPerPixel));   // a wheel notch
            return;
        } else if (dy != 0.0f) {
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

// The two ends of the one keyboard map (src/model/KeyMap.h): the codes in the
// kit-free table must be the Interface Kit's, or a binding would sit on a key
// nobody can press. These assert it at compile time.
static_assert(kKeyHome == B_HOME,          "key table: Home");
static_assert(kKeyEnd == B_END,            "key table: End (Ctrl-D's byte)");
static_assert(kKeyBackspace == B_BACKSPACE,"key table: Backspace");
static_assert(kKeyEnter == B_RETURN,       "key table: Return/Enter");
static_assert(kKeyPageUp == B_PAGE_UP,     "key table: Page Up");
static_assert(kKeyPageDown == B_PAGE_DOWN, "key table: Page Down");
static_assert(kKeyEscape == B_ESCAPE,      "key table: Escape");
static_assert(kKeyLeft == B_LEFT_ARROW,    "key table: Left arrow");
static_assert(kKeyRight == B_RIGHT_ARROW,  "key table: Right arrow");
static_assert(kKeySpace == B_SPACE,        "key table: Space");
static_assert(kKeyDelete == B_DELETE,      "key table: Delete");
static_assert(kModShift == B_SHIFT_KEY     && kModCommand == B_COMMAND_KEY
           && kModControl == B_CONTROL_KEY && kModOption == B_OPTION_KEY,
              "key table: modifier masks");

void TimelineView::KeyDown(const char* bytes, int32 numBytes) {
    if (numBytes < 1) { BView::KeyDown(bytes, numBytes); return; }
    // An open in-place rename owns the keyboard: the on-screen name field is a
    // text field, and a bare key reaching the arrangement behind it is how a
    // rename edits the wrong thing (M2.6).
    if (fRenameTrack != kInvalidTrackId && HandleRenameKey(bytes, numBytes))
        return;
    // ONE keyboard map (M3.2): the byte is resolved through the kit-free table
    // that the Keyboard Shortcuts window is also generated from, and the switch
    // is on the ACTION — no key literal is compared here any more, so the help
    // and the behaviour cannot drift.
    // The event's own modifiers (live state as the fallback), as the view's
    // other handlers read them -- a key's Shift/Cmd is the one it was pressed with.
    const KeyBinding* kb = FindKeyBinding(bytes[0], (uint32_t)EventModifiers(
        Window() ? Window()->CurrentMessage() : nullptr));
    if (kb == nullptr || kb->route != KeyRoute::Dispatch) {
        BView::KeyDown(bytes, numBytes);
        return;
    }
    // One page = the visible content width in frames.
    const Frame page = (Frame)((Bounds().right - HeaderWidth()) * fFramesPerPixel);
    switch (kb->action) {
        case KeyAction::PlayStop:
            if (BWindow* w = Window()) w->PostMessage(kMsgTransportToggle);
            break;
        case KeyAction::RecordToggle:
            if (BWindow* w = Window()) w->PostMessage(kMsgTransportRecord);
            break;
        case KeyAction::LoopToggle:          ToggleLoop(); break;
        case KeyAction::LoopBetweenMarkers:  LoopBetweenMarkers(); break;
        case KeyAction::ClickToggle:
            if (BWindow* w = Window()) w->PostMessage(kMsgClickToggle);
            break;
        case KeyAction::ReturnToZero:        ReturnToZero(); break;
        case KeyAction::PanLeft:             PanBy(-page / 4); break;
        case KeyAction::PanRight:            PanBy( page / 4); break;
        case KeyAction::MarkerPrev:          JumpToMarker(-1); break;
        case KeyAction::MarkerNext:          JumpToMarker(+1); break;
        case KeyAction::ZoomIn:              ZoomBy(0.5); break;
        case KeyAction::ZoomOut:             ZoomBy(2.0); break;
        case KeyAction::ZoomToFit:           ZoomToFit(); break;
        case KeyAction::MarkerLaneToggle:    SetMarkerLane(!fMarkerLane); break;
        case KeyAction::PageUp:
            ScrollVerticalBy(-(Bounds().Height() - ContentTop()) * 0.8f); break;
        case KeyAction::PageDown:
            ScrollVerticalBy( (Bounds().Height() - ContentTop()) * 0.8f); break;
        // The arrange tool palette's keys, one per tool (M2.3) -- the same
        // 1..N mapping the piano roll uses for its own palette.
        case KeyAction::ToolPointer:         SetTool(Tool::Pointer);  break;
        case KeyAction::ToolPencil:          SetTool(Tool::Pencil);   break;
        case KeyAction::ToolScissors:        SetTool(Tool::Scissors); break;
        case KeyAction::ToolGlue:            SetTool(Tool::Glue);     break;
        case KeyAction::ToolMute:            SetTool(Tool::Mute);     break;
        case KeyAction::ToolFade:            SetTool(Tool::Fade);     break;
        case KeyAction::GridMenu:            OpenGridMenu(); break;
        case KeyAction::SplitAtPlayhead:     SplitAtPlayhead(); break;
        // Nudge the selection one grid step (M2.5).
        case KeyAction::NudgeLeft:           NudgeSelection(-1); break;
        case KeyAction::NudgeRight:          NudgeSelection(+1); break;
        case KeyAction::DeleteSelection:
            // The range or the selected clips — whichever the user touched
            // last, which the two selections being mutually exclusive makes
            // unambiguous (item C; the rule is SelectionTargetFor).
            if (SelectionTargetFor(HasTimeRange(), !fSelClips.empty())
                != SelectTarget::None)
                RangeDelete();
            else BView::KeyDown(bytes, numBytes);
            break;
        case KeyAction::DuplicateSelection:  DuplicateSelection(); break;
        case KeyAction::ClearSelection:
            // Esc drops the clip selection first, then the range (two things
            // can be selected; one press should not silently drop both).
            if (!fSelClips.empty()) { fSelClips.clear(); Invalidate(); }
            else if (HasTimeRange()) ClearTimeRange();
            else BView::KeyDown(bytes, numBytes);
            break;
        case KeyAction::DropMarker:          DropMarker(); break;
        case KeyAction::None:
        default:
            BView::KeyDown(bytes, numBytes);
            break;
    }
}

// Home / Enter (M3.2): the playhead goes to the start and the view with it.
// Home used to only scroll, which is what the plan called out.
void TimelineView::ReturnToZero() {
    fProject->transport.playhead = 0;
    fScrollFrame = 0;
    Invalidate();
    if (BWindow* w = Window()) w->PostMessage(kMsgSeek);
}

// L (M3.2): loop on/off. Turning it on with no range adopts one, in this
// order: the range the ruler last set (the model keeps it), then the markers
// bracketing the playhead (the marker-to-marker loop, which keeps its own
// Shift-L binding), then — since package 50 — the BAR around the playhead.
// The last one exists because the toggle used to set `loopEnabled` after only
// TRYING to adopt something, so with nothing to adopt the lamp (TransportBar)
// and the status line claimed a loop that the engine ignored: a zero-length
// range loops nothing. Now the toggle either ends up with a range the engine
// can walk, or it refuses and says so.
//
// The engine walks the loop itself (M4.2), and the range it walks is part of
// the graph it is playing, so a toggle is a re-position and not an in-place
// edit: the window starts playback again from where it is, and the new graph
// walks the new range.
void TimelineView::ToggleLoop() {
    Transport& tr = fProject->transport;
    if (tr.loopEnabled) {
        tr.loopEnabled = false;
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgTransportRange);
        return;
    }
    bool have = tr.loopEnd > tr.loopStart;
    if (!have) have = LoopBetweenMarkers();
    Frame lo = tr.loopStart, hi = tr.loopEnd;
    if (!have && DefaultLoopRange(fProject->tempoMap, tr.playhead, &lo, &hi)) {
        tr.loopStart = lo;
        tr.loopEnd   = hi;
        have = true;
        // Say what was adopted: a loop the user did not draw has to name
        // itself, or "why is it playing the same bar again" has no answer.
        int bar = 1, beat = 1;
        fProject->tempoMap.BarBeat(lo, &bar, &beat);
        char msg[64];
        std::snprintf(msg, sizeof(msg), "Loop: bar %d", bar);
        if (BWindow* w = Window()) {
            BMessage st(kMsgTransportStatus);
            st.AddString("s", msg);
            w->PostMessage(&st);
        }
    }
    if (!have) {
        // Nothing to loop at all: leave the lamp off rather than light it for
        // a range the engine ignores. (The lamp is already off — the toggle
        // changes nothing in the model here.)
        if (BWindow* w = Window()) {
            BMessage st(kMsgTransportStatus);
            st.AddString("s", "Set a loop range on the ruler first");
            w->PostMessage(&st);
        }
        Invalidate();
        return;
    }
    tr.loopEnabled = true;
    Invalidate();
    // kMsgTransportRange re-reads the model for the lamp and re-positions the
    // running graph (M4.2).
    if (BWindow* w = Window()) w->PostMessage(kMsgTransportRange);
}

// M: a marker at the playhead -- the same path the marker lane's click takes
// (undoable, and the markers window follows).
void TimelineView::DropMarker() {
    AddMarkerAt(fProject->transport.playhead);
}

// --- the arrange tool palette (M2.3) --------------------------------------
//
// The strip across the top of the view: the six tools at the piano roll's
// button pitch, then the snap field and the zoom pair. Drawn with the shared
// DrawButton + icons::DrawArrangeTool, exactly as the roll's strip is, so the
// two palettes read as one control.

const char* TimelineView::ToolName(Tool t) {
    switch (t) {
        case Tool::Pointer:  return "Pointer";
        case Tool::Pencil:   return "Pencil";
        case Tool::Scissors: return "Scissors";
        case Tool::Glue:     return "Glue";
        case Tool::Mute:     return "Mute";
        default:             return "Fade";
    }
}

void TimelineView::SetTool(Tool t) {
    if (fTool == t) return;
    fTool = t;
    // The tool decides what the pointer means now: recompute the cursor and the
    // tooltip for wherever the pointer was last seen, so the change is visible
    // without moving the mouse.
    if (fHoverValid) UpdateHover(fHoverPos, 0, B_INSIDE_VIEW);
    Invalidate();
}

// Button geometry, in design pixels through Themed() so the hit targets scale
// with the font (mouse and drawing both come through here).
static constexpr float kToolBtnW = 22.0f;
static constexpr float kToolPitch = 24.0f;

BRect TimelineView::ToolRect(int i) const {
    const float x = Themed(4.0f) + i * Themed(kToolPitch);
    return BRect(x, Themed(5.0f), x + Themed(kToolBtnW), Themed(23.0f));
}
BRect TimelineView::GridRect() const {
    return BRect(Themed(158.0f), Themed(5.0f), Themed(218.0f), Themed(23.0f));
}
BRect TimelineView::ZoomOutRect() const {
    return BRect(Themed(228.0f), Themed(5.0f), Themed(250.0f), Themed(23.0f));
}
BRect TimelineView::ZoomInRect() const {
    return BRect(Themed(254.0f), Themed(5.0f), Themed(276.0f), Themed(23.0f));
}

int TimelineView::ToolButtonAt(BPoint where) const {
    for (int i = 0; i < 6; i++)
        if (ToolRect(i).Contains(where)) return i;
    return -1;
}

void TimelineView::SetSnapGrid(SnapGrid g) {
    fSnap = g;
    Invalidate(BRect(0, 0, Bounds().right, ToolbarHeight()));
    SyncScrollBars();
}

void TimelineView::HandleToolbarClick(BPoint where) {
    const int btn = ToolButtonAt(where);
    if (btn >= 0) { SetTool((Tool)btn); return; }
    if (GridRect().Contains(where)) { OpenGridMenu(); return; }
    if (ZoomOutRect().Contains(where)) { ZoomBy(2.0); return; }
    if (ZoomInRect().Contains(where))  { ZoomBy(0.5); return; }
}

// The snap popup: the plan's values (bar, 1/2 … 1/32), the triplet modifier and
// Off. Every entry comes from the kit-free SnapGrid header, so the label the
// field shows and the menu entry that set it cannot disagree.
void TimelineView::OpenGridMenu() {
    BPopUpMenu* menu = new BPopUpMenu("snap", false, false);
    int n = 0;
    const SnapDivision* div = SnapDivisions(&n);
    BMenuItem* active = nullptr;
    for (int i = 0; i < n; i++) {
        BMenuItem* it = new ThemedMenuItem(div[i].label, NULL);
        const bool on = (fSnap.kind == div[i].kind);
        it->SetMarked(on);
        if (on) active = it;
        menu->AddItem(it);
    }
    menu->AddSeparatorItem();
    BMenuItem* trip = new ThemedMenuItem("Triplets", NULL);
    trip->SetMarked(fSnap.triplet);
    menu->AddItem(trip);
    BMenuItem* off = new ThemedMenuItem("Off", NULL);
    off->SetMarked(!fSnap.On());
    if (!fSnap.On()) active = off;
    menu->AddItem(off);

    BMenuItem* sel = menu->Go(ConvertToScreen(BPoint(GridRect().left,
                                                      GridRect().bottom)), false,
                              true);
    if (sel) {
        int idx = menu->IndexOf(sel);
        if (sel == trip)          fSnap.triplet = !fSnap.triplet;
        else if (sel == off)      fSnap.kind = SnapKind::Off;
        else if (idx >= 0 && idx < n) fSnap.kind = div[idx].kind;
        SetSnapGrid(fSnap);
    }
    delete menu;
    (void)active;
}

// --- the tools' gestures ---------------------------------------------------

// Which clip/region is under `where`, and which part of it. This is the ONE
// answer the click path, the hover highlight and the cursor all use.
TimelineView::Hit TimelineView::HitTest(BPoint where) const {
    return HitTest(where, EventModifiers(Window() ? Window()->CurrentMessage()
                                                  : nullptr));
}

TimelineView::Hit TimelineView::HitTest(BPoint where, uint32 mods) const {
    Hit h;
    if (!fProject || where.x < HeaderWidth() || where.y < ContentTop())
        return h;
    const int idx = TrackIndexAt(where);
    if (idx < 0 || idx >= (int)fProject->Tracks().size()) return h;
    const Track& t = fProject->Tracks()[(size_t)idx];
    h.lane  = idx;
    h.track = t.id;

    const BRect lane = LaneRect(idx);
    const Frame at = XToFrame(where.x);
    // Audio clips and MIDI regions share the geometry; a MIDI region's is the
    // same block DrawMidiNotes paints.
    auto zoneFor = [&](Frame start, Frame len, bool midi,
                       float topBandH, float cornerW, bool withGain) -> Zone {
        const float x0 = FrameToX(start);
        const float x1 = FrameToX(start + len);
        if (where.x < x0 || where.x > x1) return Zone::None;
        const bool wide = (x1 - x0) > 2 * EdgeGrab();
        const bool topBand = where.y <= lane.top + Themed(topBandH);
        if (withGain && (mods & B_CONTROL_KEY)) return Zone::Gain;
        // The GRIPS come next, and the order is load-bearing. Alt-drag slips
        // the audio, and both Alt bits are accepted, like every other Alt
        // gesture here (the range drag, the stretch drag, exclusive solo):
        // Haiku maps a PC keyboard's Alt to B_COMMAND_KEY and the Option/
        // Windows key to B_OPTION_KEY, so testing only B_OPTION_KEY made the
        // slip edit unreachable for anyone whose Alt is the PC's. But
        // B_COMMAND_KEY is ALSO the edge/corner modifier (Cmd on the right edge
        // stretches, Cmd on a top corner sets the fade): a slip zone tested
        // before the geometry swallows those gestures whole, which is exactly
        // what the VM's TestClipStretch caught — the Cmd-drag on the right edge
        // slipped instead of stretching. So the grips win where they are, and
        // Alt means slip in the BODY.
        if (topBand && where.x <= x0 + Themed(cornerW)) return Zone::FadeIn;
        if (topBand && where.x >= x1 - Themed(cornerW)) return Zone::FadeOut;
        if (wide && where.x >= x1 - EdgeGrab()) return Zone::TrimRight;
        if (wide && where.x <= x0 + EdgeGrab()) return Zone::TrimLeft;
        if (withGain && !midi && (mods & (B_OPTION_KEY | B_COMMAND_KEY)))
            return Zone::Slip;
        return Zone::Body;
    };
    // An inactive take is hidden: not a hit target, and not the block the
    // pointer is over either.
    auto hidden = [](const Clip& c) { return c.takeGroup > 0 && !c.takeActive; };
    auto hiddenMidi = [](const MidiClip& c) { return c.takeGroup > 0 && !c.takeActive; };

    // Which block the point is on: the one DRAWN LAST (TopmostClipIndexAt).
    // Taking the first match acted on the clip underneath a crossfade — and
    // the hover highlight, the cursor and the scissors' cut line all read this
    // one answer.
    if (t.type == TrackType::Midi) {
        const int i = TopmostClipIndexAt(t.midiClips, at, hiddenMidi);
        if (i >= 0) {
            const MidiClip& c = t.midiClips[(size_t)i];
            const Zone z = zoneFor(c.startFrame, c.lengthFrames, true, 16.0f,
                                   14.0f, false);
            if (z != Zone::None) { h.clip = c.id; h.midi = true; h.zone = z; return h; }
        }
    } else {
        const int i = TopmostClipIndexAt(t.clips, at, hidden);
        if (i >= 0) {
            const Clip& c = t.clips[(size_t)i];
            const Zone z = zoneFor(c.startFrame, c.lengthFrames, false, 14.0f,
                                   12.0f, true);
            if (z != Zone::None) { h.clip = c.id; h.midi = false; h.zone = z; return h; }
        }
    }
    // An empty spot in the lane: still a real target (band select, pencil).
    h.zone = Zone::Body;
    h.clip = kInvalidClipId;
    return h;
}

TimelineView::Pointer TimelineView::CursorFor(BPoint where, uint32 mods) const {
    if (where.y < ToolbarHeight()) return Pointer::Default;
    // The lane's resize edge (M2.6) outranks the tools: the grab is in the
    // header gutter, where no tool acts.
    if (where.x < HeaderWidth() && where.y >= ContentTop()) {
        const int edge = LaneEdgeAt(where);
        if (edge >= 0 && HeaderControlAt(TrackIndexAt(where), where)
                             == HeaderControl::None)
            return Pointer::LaneSize;
    }
    switch (fTool) {
        case Tool::Scissors:
            return HitTest(where, mods).clip != kInvalidClipId ? Pointer::Split
                                                               : Pointer::Default;
        case Tool::Glue:
            return HitTest(where, mods).clip != kInvalidClipId ? Pointer::Glue
                                                               : Pointer::Default;
        case Tool::Mute:
            return (where.y >= ContentTop() && where.x >= HeaderWidth())
                 ? Pointer::Mute : Pointer::Default;
        case Tool::Pencil: {
            const Hit h = HitTest(where, mods);
            const Track* t = fProject ? fProject->FindTrack(h.track) : nullptr;
            if (t && t->type == TrackType::Midi) return Pointer::Pencil;
            return Pointer::Default;
        }
        case Tool::Fade:
            return HitTest(where, mods).clip != kInvalidClipId ? Pointer::Fade
                                                               : Pointer::Default;
        default: break;
    }
    const Hit h = HitTest(where, mods);
    if (h.clip == kInvalidClipId) return Pointer::Default;
    switch (h.zone) {
        case Zone::TrimLeft: case Zone::TrimRight: return Pointer::Trim;
        case Zone::FadeIn:   case Zone::FadeOut:   return Pointer::Fade;
        case Zone::Gain:                            return Pointer::Gain;
        case Zone::Slip:                            return Pointer::Slip;
        case Zone::Body:
            // The Alt (slip) answer for BOTH Alt bits, the same rule the zone
            // itself uses: a PC keyboard's Alt is B_COMMAND_KEY here.
            return (mods & (B_OPTION_KEY | B_COMMAND_KEY)) && !h.midi
                       ? Pointer::Slip : Pointer::Move;
        default: return Pointer::Default;
    }
}

// Scissors: cut the clicked clip at the (snapped) click. One command, the same
// Split the context menu uses.
void TimelineView::ScissorsAt(const Hit& hit, BPoint where) {
    if (hit.clip == kInvalidClipId) return;
    Frame at = Snapped(XToFrame(where.x));
    if (at < 0) at = 0;
    std::unique_ptr<Command> cmd;
    if (hit.midi) cmd = std::make_unique<SplitMidiClipCommand>(hit.track, hit.clip, at);
    else          cmd = std::make_unique<SplitClipCommand>(hit.track, hit.clip, at);
    if (fStack->Execute(std::move(cmd), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    }
}

// Glue: join the clicked clip with the next one that meets it. Clicking the
// right half of a pair glues backwards -- the user pointed at the seam, not at
// a direction, and the command refuses anything that is not a real join.
void TimelineView::GlueAt(const Hit& hit) {
    if (hit.clip == kInvalidClipId) return;
    auto join = [&](ClipId left) -> std::unique_ptr<Command> {
        if (hit.midi) return std::make_unique<JoinMidiClipsCommand>(hit.track, left);
        return std::make_unique<JoinClipsCommand>(hit.track, left);
    };
    if (fStack->Execute(join(hit.clip), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
        return;
    }
    // Backwards: the previous clip/region ends where this one starts.
    const Track* t = fProject->FindTrack(hit.track);
    if (!t) return;
    ClipId prev = kInvalidClipId;
    Frame  prevEnd = 0;
    if (hit.midi) {
        for (const MidiClip& c : t->midiClips) {
            if (c.id == hit.clip) break;
            prev = c.id; prevEnd = c.startFrame + c.lengthFrames;
        }
        if (prev == kInvalidClipId) return;
        const MidiClip* cur = t->FindMidiClip(hit.clip);
        if (!cur || prevEnd < cur->startFrame) return;
    } else {
        for (const Clip& c : t->clips) {
            if (c.id == hit.clip) break;
            prev = c.id; prevEnd = c.startFrame + c.lengthFrames;
        }
        if (prev == kInvalidClipId) return;
        const Clip* cur = t->FindClip(hit.clip);
        if (!cur || prevEnd < cur->startFrame) return;
    }
    if (fStack->Execute(join(prev), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    }
}

// Mute: the mute of the BLOCK the tool is pointed at (M2.5). Before clip
// depth existed the only mute the model had was the track's, which made the
// tool mean one thing over a block and another over the header; a clip and a
// region both carry `muted` now, so the tool says what it does. Clicking the
// empty lane still has no block to mute and does nothing.
void TimelineView::MuteAt(const Hit& hit) {
    if (hit.track == kInvalidTrackId || hit.clip == kInvalidClipId)
        return;                               // nothing under the cursor
    ToggleClipMute(hit.track, hit.clip, hit.midi);
}

// Pencil: draw a MIDI region by dragging, or one bar by clicking. Only a MIDI
// track can take one -- an audio clip needs a source, and creating one is
// M2.5/2.4 territory.
bool TimelineView::PencilAt(const Hit& hit, BPoint where) {
    if (hit.clip != kInvalidClipId) return false;   // on a region: pointer acts
    if (hit.track == kInvalidTrackId) return false;
    const Track* t = fProject->FindTrack(hit.track);
    if (!t || t->type != TrackType::Midi) return false;
    Frame start = Snapped(XToFrame(where.x));
    if (start < 0) start = 0;
    fPencilStart  = start;
    fPencilLen    = 0;         // a click makes one bar on release
    fHoverPos     = where;     // the ghost follows this until a move arrives
    fDrag         = Drag::Pencil;
    fDragTrack    = hit.track;
    fDragLane     = hit.lane;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate(LaneRect(hit.lane));
    return true;
}

void TimelineView::CommitPencil(BPoint where) {
    Track* t = fProject->FindTrack(fDragTrack);
    if (!t) return;
    Frame start = fPencilStart;
    Frame len   = fPencilLen;
    if (len <= 0) {   // a bare click: one bar, the double-click default
        const Frame one = fProject->tempoMap.FrameAt(
                              fProject->tempoMap.BeatAt(start) + 4) - start;
        len = one > 0 ? one : (Frame)(fProject->tempoMap.FramesPerBeatAt(start) * 4);
    }
    if (len < 1) len = 1;
    MidiClip nc;
    nc.startFrame   = start;
    nc.lengthFrames = len;
    auto add = std::make_unique<AddMidiClipCommand>(t->id, nc);
    AddMidiClipCommand* ap = add.get();
    if (fStack->Execute(std::move(add), *fProject)) {
        fSelClips.clear();
        fSelClips.insert(ap->CreatedId());
    }
    Invalidate();
    if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    (void)where;
}

// Fade: drag the nearer edge's fade. The top-corner grips do the same for the
// pointer tool; this makes it work anywhere in the clip, which is what a tool
// is for on a narrow region.
void TimelineView::FadeStart(const Hit& hit, BPoint where) {
    if (hit.clip == kInvalidClipId) return;
    const Track* t = fProject->FindTrack(hit.track);
    if (!t) return;
    Frame start = 0, len = 0;
    if (hit.midi) {
        const MidiClip* c = t->FindMidiClip(hit.clip);
        if (!c) return;
        start = c->startFrame; len = c->lengthFrames;
        fDragFadeInOrig  = c->fadeInFrames;
        fDragFadeOutOrig = c->fadeOutFrames;
    } else {
        const Clip* c = t->FindClip(hit.clip);
        if (!c) return;
        start = c->startFrame; len = c->lengthFrames;
        fDragFadeInOrig  = c->fadeInFrames;
        fDragFadeOutOrig = c->fadeOutFrames;
    }
    const float x0 = FrameToX(start), x1 = FrameToX(start + len);
    fDrag = (where.x - x0) <= (x1 - where.x) ? Drag::ClipFadeIn
                                             : Drag::ClipFadeOut;
    fDragTrack       = hit.track;
    fDragLane        = hit.lane;
    fDragClip        = hit.clip;
    fDragIsMidiClip  = hit.midi;
    fDragClipOrig    = start;
    fDragClipOrigLen = len;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    PreviewDrag(where);
}

// Split every clip/region the playhead falls strictly inside, as ONE undo step.
void TimelineView::SplitAtPlayhead() {
    if (!fProject || !fStack) return;
    const Frame at = fProject->transport.playhead;
    if (at < 0) return;
    // Collect first: the commands mutate the model when the macro runs, and
    // walking it while it changes would be a different set every time.
    struct Target { TrackId track; ClipId clip; bool midi; };
    std::vector<Target> targets;
    for (const Track& t : fProject->Tracks()) {
        for (const Clip& c : t.clips) {
            if (c.takeGroup > 0 && !c.takeActive) continue;
            if (at > c.startFrame && at < c.startFrame + c.lengthFrames
                && (fSelClips.empty() || ClipSelected(c.id)))
                targets.push_back({ t.id, c.id, false });
        }
        for (const MidiClip& c : t.midiClips) {
            if (c.takeGroup > 0 && !c.takeActive) continue;
            if (at > c.startFrame && at < c.startFrame + c.lengthFrames
                && (fSelClips.empty() || ClipSelected(c.id)))
                targets.push_back({ t.id, c.id, true });
        }
    }
    if (targets.empty()) return;
    auto macro = std::make_unique<MacroCommand>("Split at Playhead");
    for (const Target& tg : targets) {
        if (tg.midi) macro->Add(std::make_unique<SplitMidiClipCommand>(tg.track, tg.clip, at));
        else         macro->Add(std::make_unique<SplitClipCommand>(tg.track, tg.clip, at));
    }
    if (fStack->Execute(std::move(macro), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    }
}

// --- hover, cursors and tooltips (M2.1) ------------------------------------

// The header's controls, in the order the drawing and the click path agree on
// (M2.6). A folded lane draws the compact set, so which rects apply depends on
// the track's state -- one function answers for both.
TimelineView::HeaderControl TimelineView::HeaderControlAt(int idx,
                                                          BPoint where) const {
    if (idx < 0 || !fProject || idx >= (int)fProject->Tracks().size())
        return HeaderControl::None;
    const Track& t = fProject->Tracks()[(size_t)idx];
    const BRect lane = LaneRect(idx);
    if (t.collapsed) {
        if (FoldRectCompact(lane).Contains(where))          return HeaderControl::Fold;
        if (ColourSwatchRectCompact(lane).Contains(where))  return HeaderControl::Colour;
        if (MuteRectCompact(lane).Contains(where))          return HeaderControl::Mute;
        if (SoloRectCompact(lane).Contains(where))          return HeaderControl::Solo;
        return HeaderControl::None;
    }
    if (FoldRect(lane).Contains(where))          return HeaderControl::Fold;
    if (MuteRect(lane).Contains(where))          return HeaderControl::Mute;
    if (SoloRect(lane).Contains(where))          return HeaderControl::Solo;
    if (ArmRect(lane).Contains(where))           return HeaderControl::Arm;
    if (MonRect(lane).Contains(where))           return HeaderControl::Monitor;
    if (PanKnobRect(lane).Contains(where))       return HeaderControl::Pan;
    if (GainRect(lane).Contains(where))          return HeaderControl::Gain;
    if (ColourSwatchRect(lane).Contains(where))  return HeaderControl::Colour;
    if (InputRect(lane).Contains(where))         return HeaderControl::Input;
    return HeaderControl::None;
}

BRect TimelineView::HeaderRectFor(int lane, HeaderControl which) const {
    if (!fProject || lane < 0 || lane >= (int)fProject->Tracks().size())
        return BRect();
    const Track& t = fProject->Tracks()[(size_t)lane];
    const BRect l = LaneRect(lane);
    const bool compact = t.collapsed;
    switch (which) {
        case HeaderControl::Fold: return compact ? FoldRectCompact(l)
                                                 : FoldRect(l);
        case HeaderControl::Colour: return compact ? ColourSwatchRectCompact(l)
                                                   : ColourSwatchRect(l);
        case HeaderControl::Mute: return compact ? MuteRectCompact(l) : MuteRect(l);
        case HeaderControl::Solo: return compact ? SoloRectCompact(l) : SoloRect(l);
        case HeaderControl::Arm:  return ArmRect(l);
        case HeaderControl::Monitor: return MonRect(l);
        case HeaderControl::Pan:  return PanKnobRect(l);
        case HeaderControl::Gain: return GainRect(l);
        case HeaderControl::Input: return InputRect(l);
        default: return BRect();
    }
}

// The lane whose bottom edge is in the grab band, in the header gutter only:
// the lane content's own bottom pixels belong to clips and ranges, and the
// header is where every other per-lane control already lives.
int TimelineView::LaneEdgeAt(BPoint where) const {
    if (!fProject || where.x >= HeaderWidth()) return -1;
    const float grab = Themed(3.0f);
    const int n = (int)fProject->Tracks().size();
    for (int i = 0; i < n; i++) {
        const float bottom = LaneRect(i).bottom;
        if (where.y >= bottom - grab && where.y <= bottom + TrackGap())
            return i;
    }
    return -1;
}

// The tooltip for a header control.
static const char* HeaderTip(TimelineView::HeaderControl which) {
    switch (which) {
        case TimelineView::HeaderControl::Mute: return "Mute";
        case TimelineView::HeaderControl::Solo:
            return "Solo (Alt-click: solo this track only)";
        case TimelineView::HeaderControl::Arm:  return "Record arm";
        case TimelineView::HeaderControl::Monitor: return "Input monitor";
        case TimelineView::HeaderControl::Pan:  return "Pan (drag up/down)";
        case TimelineView::HeaderControl::Gain: return "Volume";
        case TimelineView::HeaderControl::Colour: return "Track colour";
        case TimelineView::HeaderControl::Input:  return "Record input";
        case TimelineView::HeaderControl::Fold:
            return "Collapse / expand the lane";
        default: return "";
    }
}

// A short label for whatever the pointer is over: the palette buttons, the
// header controls, the clip edges and the ruler.
void TimelineView::UpdateHover(BPoint where, uint32 mods, uint32 transit) {
    if (mods == 0 && Window() && Window()->CurrentMessage())
        mods = EventModifiers(Window()->CurrentMessage());
    if (transit == B_EXITED_VIEW) {
        fHoverValid = false;
        fHover = Hit();
        fHoverToolBtn = -1;
        fHoverHeader = HeaderControl::None;
        fHoverLaneEdge = false;
        fSplitHoverX = -1.0f;
        SetTip("");
        ApplyCursor(Pointer::Default);
        Invalidate();
        return;
    }

    const Hit prevHit = fHover;
    const int prevBtn  = fHoverToolBtn;
    const HeaderControl prevHdr = fHoverHeader;
    const bool prevEdge = fHoverLaneEdge;
    const float prevSplit = fSplitHoverX;

    fHoverPos = where;
    fHoverValid = true;
    fHoverToolBtn = (where.y < ToolbarHeight()) ? ToolButtonAt(where) : -1;
    fHoverHeader = HeaderControl::None;
    fHoverLaneEdge = false;
    fHover = Hit();
    fSplitHoverX = -1.0f;

    const char* tip = "";
    if (where.y < ToolbarHeight()) {
        if (fHoverToolBtn >= 0)       tip = ToolName((Tool)fHoverToolBtn);
        else if (GridRect().Contains(where))
            tip = fSnap.On() ? "Snap: click to change the grid"
                             : "Snap is off: click to choose a grid";
        else if (ZoomOutRect().Contains(where)) tip = "Zoom out";
        else if (ZoomInRect().Contains(where))  tip = "Zoom in";
    } else if (fMarkerLane && where.y >= RulerBottom() && where.y < ContentTop()) {
        // The marker lane (M2.7): flag = seek, drag it to move it, empty =
        // add on double-click, right-click a flag for rename/delete.
        tip = MarkerInLane(where)
            ? "Marker: click to jump, drag to move, right-click for Rename/Delete"
            : "Markers: double-click to add one here";
    } else if (where.y < RulerBottom()) {
        // The ruler (RulerBottom, not ContentTop: the marker lane sits below).
        tip = MarkerAt(where)
            ? "Marker: click to jump, right-click for the marker menu"
            : "Ruler: click to seek, drag for a loop, "
              "Ctrl-drag for punch, Alt-drag for a time range";
    } else if (where.x < HeaderWidth()) {
        // The header: a control highlight, and the track it belongs to (the
        // drawing compares ids).
        const int hidx = TrackIndexAt(where);
        fHover.lane = hidx;
        if (hidx >= 0 && hidx < (int)fProject->Tracks().size())
            fHover.track = fProject->Tracks()[(size_t)hidx].id;
        fHoverHeader = HeaderControlAt(hidx, where);
        if (fHoverHeader != HeaderControl::None) {
            tip = HeaderTip(fHoverHeader);
        } else {
            // The lane's bottom edge: the resize grab (M2.6).
            const int edge = LaneEdgeAt(where);
            if (edge >= 0) {
                fHoverLaneEdge = true;
                if (fHover.lane < 0) fHover.lane = edge;
                tip = "Drag to resize the lane";
            }
        }
    } else {
        fHover = HitTest(where);
        if (fHover.clip != kInvalidClipId) {
            fSplitHoverX = FrameToX(Snapped(XToFrame(where.x)));
            switch (fTool) {
                case Tool::Scissors: tip = "Split here"; break;
                case Tool::Glue:     tip = "Glue with the next clip"; break;
                // The tool mutes the BLOCK under the pointer (M2.5's Clip.muted
                // / MidiClip.muted) -- not the whole track, which is what the
                // header's M button is for.
                case Tool::Mute:     tip = "Mute this clip"; break;
                case Tool::Pencil:   tip = "Drag to draw a region"; break;
                case Tool::Fade:     tip = "Drag to set the fade"; break;
                default:
                    switch (fHover.zone) {
                        case Zone::TrimLeft:  tip = "Trim the clip start"; break;
                        // Cmd/Alt on this edge stretches instead of trimming
                        // (M7.2): say so while the modifier is down, or the
                        // gesture is undiscoverable.
                        case Zone::TrimRight:
                            tip = (mods & (B_COMMAND_KEY | B_OPTION_KEY))
                                ? "Stretch the clip to fit (the audio keeps its pitch)"
                                : "Trim the clip end (Cmd-drag: stretch to fit)";
                            break;
                        case Zone::FadeIn:    tip = "Fade in"; break;
                        case Zone::FadeOut:   tip = "Fade out"; break;
                        case Zone::Gain:      tip = "Clip gain (drag up/down)"; break;
                        case Zone::Slip:      tip = "Slip the audio inside the clip"; break;
                        default:
                            tip = fHover.midi ? "Region: drag to move, "
                                                "double-click to edit"
                                              : "Clip: drag to move";
                            break;
                    }
            }
        } else if (fTool == Tool::Pencil) {
            const Track* t = fProject ? fProject->FindTrack(fHover.track) : nullptr;
            if (t && t->type == TrackType::Midi) tip = "Drag to draw a region";
        } else if (fTool == Tool::Pointer) {
            tip = "Drag to select a time range (Shift: band-select clips)";
        }
    }
    SetTip(tip);
    ApplyCursor(CursorFor(where, mods));

    // Repaint only what the feedback covers: the lanes whose highlight changed,
    // plus the two columns the scissors' cut line moved between.
    const bool hitChanged = prevHit.clip != fHover.clip
                            || prevHit.lane != fHover.lane
                            || prevHit.zone != fHover.zone;
    if (hitChanged || prevBtn != fHoverToolBtn || prevHdr != fHoverHeader
        || prevEdge != fHoverLaneEdge) {
        if (prevHit.lane >= 0) Invalidate(LaneRect(prevHit.lane));
        if (fHover.lane  >= 0) Invalidate(LaneRect(fHover.lane));
        if (prevHdr != fHoverHeader && fHoverHeader == HeaderControl::None
            && fHover.lane >= 0)
            Invalidate(LaneRect(fHover.lane));
        if (where.y < ToolbarHeight() || prevBtn >= 0)
            Invalidate(BRect(0, 0, Bounds().right, ToolbarHeight()));
    } else if (fTool == Tool::Scissors && fSplitHoverX >= 0.0f
               && fSplitHoverX != prevSplit && fHover.lane >= 0) {
        const BRect lane = LaneRect(fHover.lane);
        Invalidate(BRect(prevSplit - Themed(2.0f), lane.top,
                         prevSplit + Themed(2.0f), lane.bottom));
        Invalidate(BRect(fSplitHoverX - Themed(2.0f), lane.top,
                         fSplitHoverX + Themed(2.0f), lane.bottom));
    }
}

void TimelineView::SetTip(const char* text) {
    if (fTip == text) return;
    fTip = text;
    if (fTip.empty()) { HideToolTip(); return; }
    SetToolTip(fTip.c_str());
    ShowToolTip();
}

// The cursor objects are built once, on first use: a BCursor needs the app
// server, so they cannot be static, and the glyph cursors need a bitmap draw.
const BCursor* TimelineView::CursorObject(Pointer p) {
    auto it = fCursors.find((int)p);
    if (it != fCursors.end()) return it->second.get();
    std::unique_ptr<BCursor> c;
    switch (p) {
        case Pointer::Move:  c.reset(new BCursor(B_CURSOR_ID_GRAB)); break;
        case Pointer::Trim:  c.reset(new BCursor(B_CURSOR_ID_RESIZE_EAST_WEST)); break;
        case Pointer::Gain:  c.reset(new BCursor(B_CURSOR_ID_RESIZE_NORTH_SOUTH)); break;
        case Pointer::LaneSize:
            c.reset(new BCursor(B_CURSOR_ID_RESIZE_NORTH_SOUTH));
            break;
        case Pointer::Split: c.reset(new BCursor(B_CURSOR_ID_CROSS_HAIR)); break;
        case Pointer::Default:
            c.reset(new BCursor(B_CURSOR_ID_SYSTEM_DEFAULT));
            break;
        default: {
            // A tool's own glyph, drawn with the same code its button uses.
            int glyph = -1;
            switch (p) {
                case Pointer::Fade:   glyph = 5; break;
                case Pointer::Pencil: glyph = 1; break;
                case Pointer::Glue:   glyph = 3; break;
                case Pointer::Mute:   glyph = 4; break;
                default: break;
            }
            if (glyph < 0 && p != Pointer::Slip) return nullptr;
            // B_BITMAP_ACCEPTS_VIEWS is what gives the drawing view an owner:
            // a bitmap without an off-screen window has no fWindow, so AddChild
            // silently does nothing and EVERY BView call the glyph makes then
            // hits "View method requires owner and doesn't have one" -- a crash
            // dialog on the target, not a failed check. The bitmap's window must
            // be locked before the child is added.
            BBitmap* bm = new BBitmap(BRect(0, 0, 15, 15), B_RGBA32, true);
            if (!bm->IsValid() || !bm->Lock()) { delete bm; return nullptr; }
            memset(bm->Bits(), 0, (size_t)bm->BitsLength());
            BView* v = new BView(bm->Bounds(), "cur", 0, 0);
            bm->AddChild(v);
            v->SetDrawingMode(B_OP_COPY);
            auto paint = [&](rgb_color c) {
                if (p == Pointer::Slip) icons::DrawSlipGlyph(v, bm->Bounds(), c);
                else icons::DrawArrangeTool(v, bm->Bounds(), glyph, c);
            };
            // A one-pixel black halo first, so the glyph reads on any
            // background, then the glyph itself in white.
            const BPoint ring[4] = { BPoint(1, 0), BPoint(-1, 0), BPoint(0, 1),
                                     BPoint(0, -1) };
            for (const BPoint& o : ring) {
                v->SetOrigin(o);
                v->SetHighColor(0, 0, 0, 255);
                paint(Rgb(0, 0, 0));
            }
            v->SetOrigin(BPoint(0, 0));
            v->SetHighColor(255, 255, 255, 255);
            paint(Rgb(255, 255, 255));
            bm->Unlock();
            c.reset(new BCursor(bm, BPoint(2, 2)));
            delete bm;
            break;
        }
    }
    if (!c || c->InitCheck() != B_OK) return nullptr;
    const BCursor* out = c.get();
    fCursors[(int)p] = std::move(c);
    return out;
}

void TimelineView::ApplyCursor(Pointer p) {
    if (p == fCursor) return;
    const BCursor* c = CursorObject(p);
    if (!c) return;
    fCursor = p;
    SetViewCursor(c, true);
}

void TimelineView::DrawHoverEdge(const BRect& block, Zone zone) {
    if (zone == Zone::None || zone == Zone::Body) return;
    SetHighColor(ColAccent());
    const float w = Themed(3.0f);
    switch (zone) {
        case Zone::TrimLeft:
            FillRect(BRect(block.left, block.top, block.left + w, block.bottom));
            break;
        case Zone::TrimRight:
            FillRect(BRect(block.right - w, block.top, block.right, block.bottom));
            break;
        case Zone::FadeIn: {
            BPoint p[3] = { BPoint(block.left + Themed(1), block.top + Themed(1)),
                            BPoint(block.left + Themed(2) + w * 3, block.top + Themed(1)),
                            BPoint(block.left + Themed(1), block.top + Themed(2) + w * 3) };
            FillPolygon(p, 3);
            break;
        }
        case Zone::FadeOut: {
            BPoint p[3] = { BPoint(block.right - Themed(1), block.top + Themed(1)),
                            BPoint(block.right - Themed(2) - w * 3, block.top + Themed(1)),
                            BPoint(block.right - Themed(1), block.top + Themed(2) + w * 3) };
            FillPolygon(p, 3);
            break;
        }
        case Zone::Gain:
            StrokeRect(block);
            break;
        case Zone::Slip:
            StrokeRoundRect(block.InsetByCopy(Themed(1), Themed(1)), Themed(5),
                            Themed(5));
            break;
        default:
            break;
    }
}

// With the scissors selected, the clip under the pointer shows where the cut
// would land -- the pointer's intent, drawn before the click.
void TimelineView::DrawScissorsHover(const BRect& block) {
    if (fTool != Tool::Scissors) return;
    if (fHover.clip == kInvalidClipId || fSplitHoverX < block.left
        || fSplitHoverX > block.right)
        return;
    SetHighColor(ColAccent());
    const float dash = Themed(4.0f);
    for (float y = block.top; y < block.bottom; y += dash * 2.0f)
        StrokeLine(BPoint(fSplitHoverX, y),
                   BPoint(fSplitHoverX, std::min(y + dash, block.bottom)));
}

// --- the scrollbars (M2.2) --------------------------------------------------

void TimelineView::LayoutScrollBars() {
    if (!fHBar || !fVBar) return;
    const float w = Bounds().Width(), h = Bounds().Height();
    const float b = fBarThickness;
    fHBar->MoveTo(0.0f, h - b);
    fHBar->ResizeTo(std::max(1.0f, w - b), b);
    fVBar->MoveTo(w - b, 0.0f);
    fVBar->ResizeTo(b, std::max(1.0f, h - b));
}

void TimelineView::FrameResized(float, float) {
    // A B_SUPPORTS_LAYOUT view does not reposition children by their follow
    // modes (BView::_ResizeBy), so this is where the scrollbars land.
    LayoutScrollBars();
    SyncScrollBars();
    if (fHoverValid) UpdateHover(fHoverPos, 0, B_INSIDE_VIEW);
}

void TimelineView::AttachedToWindow() {
    MakeFocus(true);   // receive arrow/zoom keys
    LayoutScrollBars();
    SyncScrollBars();
}

// Ranges, proportions and values, only when they actually changed: this runs on
// every repaint, and BScrollBar::SetValue would otherwise push thumb updates to
// the server for nothing.
void TimelineView::SyncScrollBars() {
    if (!fHBar || !fVBar || fSyncingBars) return;
    fSyncingBars = true;

    const float visibleW = std::max(1.0f, Bounds().Width() - HeaderWidth()
                                           - fBarThickness);
    const Frame endFrame = ContentEndFrame();
    const float visibleFrames = (float)(visibleW * fFramesPerPixel);
    const float totalFrames   = (float)endFrame + visibleFrames;   // > 0
    const float maxScroll     = (float)endFrame;
    const float propX = totalFrames > 0.0f ? visibleFrames / totalFrames : 1.0f;
    if (maxScroll != fBarLastRangeX) {
        fHBar->SetRange(0.0f, maxScroll);
        fBarLastRangeX = maxScroll;
    }
    // The arrow and trough steps are in the SAME units as the range — frames —
    // so they have to follow the zoom: the constructor's stock 12/120 moved the
    // view by twelve frames, a fiftieth of a pixel at the default zoom, i.e.
    // nothing at all (package 50, item E). Small = a visible 16 px nudge,
    // large = one screenful; both from the kit-free rule so the host tests can
    // pin the arithmetic.
    float stepX = 12.0f, pageX = 120.0f;
    ScrollStepsFor(fFramesPerPixel, visibleW, &stepX, &pageX);
    if (stepX != fBarLastStepX || pageX != fBarLastPageX) {
        fHBar->SetSteps(stepX, pageX);
        fBarLastStepX = stepX;
        fBarLastPageX = pageX;
    }
    if (propX != fBarLastPropX) {
        fHBar->SetProportion(std::min(1.0f, propX));
        fBarLastPropX = propX;
    }
    if ((float)fScrollFrame != fBarLastX) {
        fHBar->SetValue((float)fScrollFrame);
        fBarLastX = (float)fScrollFrame;
    }

    // Same visible height ScrollToY clamps with (the horizontal scrollbar's
    // strip is not lane space), so the thumb cannot run past where the view
    // can actually scroll.
    const float viewH = std::max(1.0f,
        Bounds().Height() - ContentTop() - fBarThickness);
    const float contentH = ContentHeight();
    const float maxY = std::max(0.0f, contentH - viewH);
    const float propY = contentH > 0.0f ? std::min(1.0f, viewH / contentH) : 1.0f;
    if (maxY != fBarLastRangeY) {
        fVBar->SetRange(0.0f, maxY);
        fBarLastRangeY = maxY;
    }
    if (propY != fBarLastPropY) {
        fVBar->SetProportion(propY);
        fBarLastPropY = propY;
    }
    if (fScrollY != fBarLastY) {
        fVBar->SetValue(fScrollY);
        fBarLastY = fScrollY;
    }
    fSyncingBars = false;
}

// --- the tool strip's drawing ----------------------------------------------

void TimelineView::DrawToolButton(int i) {
    const BRect r = ToolRect(i);
    const bool active = (i == (int)fTool);
    DrawButton(this, r, "", active, ColAccent());
    icons::DrawArrangeTool(this, r.InsetByCopy(Themed(4.0f), Themed(3.0f)), i,
                           active ? Rgb(16, 18, 22) : ColText());
    if (!active && i == fHoverToolBtn) {   // hover: the palette's own highlight
        SetHighColor(ColAccent());
        StrokeRoundRect(r, Themed(3.0f), Themed(3.0f));
    }
}

void TimelineView::DrawToolbar() {
    const float h = ToolbarHeight();
    SetHighColor(ColHeader());
    FillRect(BRect(0, 0, Bounds().right, h - 1));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, h - 1), BPoint(Bounds().right, h - 1));

    for (int i = 0; i < 6; i++)
        DrawToolButton(i);

    // Group separators, then the snap field and the zoom pair.
    SetHighColor(ColGrid());
    StrokeLine(BPoint(Themed(150.0f), Themed(6.0f)),
               BPoint(Themed(150.0f), Themed(22.0f)));
    StrokeLine(BPoint(Themed(222.0f), Themed(6.0f)),
               BPoint(Themed(222.0f), Themed(22.0f)));

    // The snap indicator: the field's LABEL is the grid that is active, and the
    // button is lit only while snapping is on -- so "off" reads at a glance.
    DrawButton(this, GridRect(), SnapGridLabel(fSnap), fSnap.On(), ColAccent());
    DrawButton(this, ZoomOutRect(), "\xE2\x88\x92", false);   // minus
    DrawButton(this, ZoomInRect(),  "+", false);
}

Grid TimelineView::GridOf() const {
    Grid g;
    g.sampleRate  = fProject->sampleRate;
    g.tempoBPM    = fProject->tempoBPM;
    g.beatsPerBar = fProject->timeSig.numerator;
    return g;
}

Frame TimelineView::Snapped(Frame f) const {
    if (EventModifiers(Window() ? Window()->CurrentMessage() : nullptr)
        & B_SHIFT_KEY)   // hold Shift for free placement
        return f;
    if (!fProject) return f < 0 ? 0 : f;
    // The grid the snap menu selected, through the tempo map (M2.3). It used to
    // be a hard-coded 16th; Shift is still the free-placement override.
    return SnapFrame(fProject->tempoMap, f, fSnap);
}

// The ONE block menu: an audio clip and a MIDI region share it, and `midi`
// drops the entries the model cannot serve for a region — there is no `name`
// field to rename, no fade SHAPE (a region's fades are velocity ramps),
// nothing decoded for Normalize/Reverse/Strip Silence, and
// SetClipColorCommand only knows audio clips. One list, so the two cannot
// drift apart again: the region's menu used to be a three-item stub while the
// clip's had everything, even though a region mutes and fades like a clip.
//
// Returns the chosen item's label ("" if dismissed) so callers switch on
// meaning, not a fragile index.
std::string TimelineView::ClipMenu(BPoint where, bool midi, bool withTake) const {
    BPopUpMenu* m = new BPopUpMenu("clip", false, false);
    m->AddItem(new ThemedMenuItem("Copy", NULL));
    m->AddItem(new ThemedMenuItem("Delete", NULL));
    m->AddItem(new ThemedMenuItem("Split here", NULL));
    if (withTake)
        m->AddItem(new ThemedMenuItem("Next Take", NULL));
    m->AddSeparatorItem();

    // The block under the cursor, for the state-dependent labels — through the
    // SAME draw-order rule the click path uses (TopmostClipIndexAt), or the
    // menu would describe the block underneath a crossfade.
    const Clip* cur = nullptr;
    const MidiClip* curM = nullptr;
    if (const int idx = TrackIndexAt(where);
        idx >= 0 && idx < (int)fProject->Tracks().size()) {
        const Track& t = fProject->Tracks()[(size_t)idx];
        const Frame at = XToFrame(where.x);
        if (midi) {
            auto skip = [](const MidiClip& c) {
                return c.takeGroup > 0 && !c.takeActive;
            };
            const int i = TopmostClipIndexAt(t.midiClips, at, skip);
            if (i >= 0) curM = &t.midiClips[(size_t)i];
        } else {
            auto skip = [](const Clip& c) {
                return c.takeGroup > 0 && !c.takeActive;
            };
            const int i = TopmostClipIndexAt(t.clips, at, skip);
            if (i >= 0) cur = &t.clips[(size_t)i];
        }
    }
    const bool muted = midi ? (curM && curM->muted) : (cur && cur->muted);

    // Clip depth (M2.5): the fields with no other gesture, plus the mute the
    // Mute tool also toggles.
    if (!midi) {
        m->AddItem(new ThemedMenuItem("Rename Clip" B_UTF8_ELLIPSIS, NULL));
        m->AddItem(new ThemedMenuItem("Next Colour", NULL));
    }
    m->AddItem(new ThemedMenuItem(muted ? "Unmute Clip" : "Mute Clip", NULL));
    if (!midi) {
        BMenu* fs = new BMenu("Fade Shape");
        const char* shapes[] = { "Fade Linear", "Fade Equal Power", "Fade S-Curve" };
        for (int i = 0; i < 3; i++) {
            BMenuItem* it = new ThemedMenuItem(shapes[i], NULL);
            it->SetMarked(cur && (int)cur->fadeShape == i);
            fs->AddItem(it);
        }
        m->AddItem(fs);
    }
    m->AddSeparatorItem();
    if (!midi) {
        m->AddItem(new ThemedMenuItem("Normalize", NULL));
        m->AddItem(new ThemedMenuItem("Reverse", NULL));
        m->AddItem(new ThemedMenuItem("Strip Silence", NULL));
    }
    m->AddItem(new ThemedMenuItem("Clear Fades", NULL));
    BMenuItem* sel = m->Go(const_cast<TimelineView*>(this)->ConvertToScreen(where),
                           false, true);
    std::string label = sel ? std::string(sel->Label()) : std::string();
    delete m;
    return label;
}

// The lane area's one context menu (package 50): the empty part of a lane, the
// 1 px gap between two lanes, and everything below the last lane. Before it,
// an empty-lane right-click offered "Paste here" only with a non-empty
// clipboard and nothing at all otherwise, and a click in the gap or the void
// was silently swallowed.
//
// `track` is the track under the cursor (an invalid id below the last lane):
// Paste lands on it at the clicked frame when there is one, else at the
// playhead through the ordinary paste-target rule.
void TimelineView::LaneAreaMenu(BPoint where, TrackId track) {
    BPopUpMenu* m = new BPopUpMenu("lane", false, false);
    BMenuItem* paste = new ThemedMenuItem("Paste", NULL);
    paste->SetEnabled(!fClipboard.empty());    // no clipboard: nothing to paste
    m->AddItem(paste);
    m->AddItem(new ThemedMenuItem("Select All", NULL));
    BMenuItem* silence = new ThemedMenuItem("Insert Silence", NULL);
    silence->SetEnabled(HasTimeRange());       // it acts on the time range
    m->AddItem(silence);
    m->AddSeparatorItem();
    m->AddItem(new ThemedMenuItem("Zoom to Fit", NULL));
    m->AddSeparatorItem();
    m->AddItem(new ThemedMenuItem("New Audio Track", NULL));
    m->AddItem(new ThemedMenuItem("New MIDI Track", NULL));
    BMenuItem* sel = m->Go(ConvertToScreen(where), false, true);
    const std::string pick = sel ? std::string(sel->Label()) : std::string();
    delete m;
    if (pick.empty()) return;
    if (pick == "Paste") {
        const Track* t = fProject->FindTrack(track);
        if (t)
            PasteToTrack(track, Snapped(XToFrame(where.x)), t->type);
        else
            PasteAtPlayhead();
    } else if (pick == "Select All") {
        SelectAllClips();
    } else if (pick == "Insert Silence") {
        RangeInsertSilence();
    } else if (pick == "Zoom to Fit") {
        ZoomToFit();
    } else if (pick == "New Audio Track") {
        if (BWindow* w = Window()) w->PostMessage(MSG_NEW_AUDIO);
    } else if (pick == "New MIDI Track") {
        if (BWindow* w = Window()) w->PostMessage(MSG_NEW_MIDI);
    }
    Invalidate();
}

// --- the clipboard (M2.4) --------------------------------------------------
//
// Many clips, since a range copy brings a whole slice of the arrangement with
// it. The order the items were collected in is preserved and every start is
// relative to the earliest item, so a paste keeps the range's internal spacing.

void TimelineView::SetClipboardToClip(const Clip& c) {
    fClipboard.clear();
    ClipItem it;
    it.midi = false;
    it.audio = c;
    it.audio.startFrame = 0;
    fClipboard.push_back(it);
}

void TimelineView::SetClipboardToRegion(const MidiClip& c) {
    fClipboard.clear();
    ClipItem it;
    it.midi = true;
    it.region = c;
    it.region.startFrame = 0;
    fClipboard.push_back(it);
}

// The clip selection as clipboard items: starts relative to the selection's
// earliest clip, so a multi-clip copy pastes as a group.
void TimelineView::SetClipboardToSelection() {
    Frame minStart = 0;
    bool have = false;
    for (ClipId id : fSelClips)
        if (Track* t = TrackOfClip(id)) {
            if (Clip* c = t->FindClip(id)) {
                if (!have || c->startFrame < minStart) minStart = c->startFrame;
                have = true;
            } else if (MidiClip* m = t->FindMidiClip(id)) {
                if (!have || m->startFrame < minStart) minStart = m->startFrame;
                have = true;
            }
        }
    if (!have) return;
    fClipboard.clear();
    for (ClipId id : fSelClips)
        if (Track* t = TrackOfClip(id)) {
            if (Clip* c = t->FindClip(id)) {
                ClipItem it;
                it.midi = false;
                it.track = t->id;
                it.audio = *c;
                it.audio.startFrame = c->startFrame - minStart;
                fClipboard.push_back(it);
            } else if (MidiClip* m = t->FindMidiClip(id)) {
                ClipItem it;
                it.midi = true;
                it.track = t->id;
                it.region = *m;
                it.region.startFrame = m->startFrame - minStart;
                fClipboard.push_back(it);
            }
        }
}

// Where a paste lands on a track: the selected track when its kind matches,
// else the first track that does (the rule PasteAtPlayhead always used, now
// with "the selected track" first because the plan names it).
TrackId TimelineView::PasteTargetFor(bool midi) const {
    const TrackType want = midi ? TrackType::Midi : TrackType::Audio;
    if (const Track* sel = fProject->FindTrack(fSelectedTrack))
        if (sel->type == want) return sel->id;
    for (const Track& t : fProject->Tracks())
        if (t.type == want) return t.id;
    return kInvalidTrackId;
}

// Drop the clipboard's items on their tracks, `at` frames after the position
// each was copied from. One MacroCommand: a paste is one undo step.
void TimelineView::PasteItems(const std::vector<ClipItem>& items, Frame at,
                              bool toSourceTracks) {
    if (items.empty()) return;
    auto macro = std::make_unique<MacroCommand>("Paste");
    for (const ClipItem& it : items) {
        const TrackId track = (toSourceTracks && it.track != kInvalidTrackId
                               && fProject->FindTrack(it.track))
                              ? it.track : PasteTargetFor(it.midi);
        if (track == kInvalidTrackId) continue;
        if (it.midi) {
            MidiClip c = it.region;
            c.id = kInvalidClipId;      // AddMidiClipCommand assigns a fresh id
            c.startFrame += at;
            if (c.startFrame < 0) c.startFrame = 0;
            macro->Add(std::make_unique<AddMidiClipCommand>(track, c));
        } else {
            Clip c = it.audio;
            c.id = kInvalidClipId;      // AddClipCommand assigns a fresh id
            c.startFrame += at;
            if (c.startFrame < 0) c.startFrame = 0;
            macro->Add(std::make_unique<AddClipCommand>(track, c));
        }
    }
    if (macro->Empty()) return;
    if (fStack->Execute(std::move(macro), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    }
}

// Right-click "Paste here": the same items, from `at`, but forced onto the
// track that was clicked (the user pointed at it).
void TimelineView::PasteToTrack(TrackId track, Frame at, TrackType type) {
    if (track == kInvalidTrackId || fClipboard.empty()) return;
    auto macro = std::make_unique<MacroCommand>("Paste");
    for (const ClipItem& it : fClipboard) {
        if (it.midi) {
            if (type != TrackType::Midi) continue;
            MidiClip c = it.region;
            c.id = kInvalidClipId;
            c.startFrame += at;
            if (c.startFrame < 0) c.startFrame = 0;
            macro->Add(std::make_unique<AddMidiClipCommand>(track, c));
        } else {
            if (type != TrackType::Audio) continue;
            Clip c = it.audio;
            c.id = kInvalidClipId;
            c.startFrame += at;
            if (c.startFrame < 0) c.startFrame = 0;
            macro->Add(std::make_unique<AddClipCommand>(track, c));
        }
    }
    if (macro->Empty()) return;
    if (fStack->Execute(std::move(macro), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
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
    PasteItems(fClipboard, at < 0 ? 0 : at);
}

// --- the time range (M2.4) -------------------------------------------------

void TimelineView::SetTimeRange(Frame a, Frame b) {
    if (a > b) std::swap(a, b);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (fRangeValid && fRangeA == a && fRangeB == b) return;
    ClearTimeRange();          // one invalidate, not two
    if (b <= a) return;        // an empty span is no range at all
    fRangeA = a;
    fRangeB = b;
    fRangeValid = true;
    // Setting a range drops the clip selection: the two selections are
    // mutually exclusive — the gesture the user just made is the one every
    // operation acts on (package 50, item C; `SelectionTargetFor` is the rule
    // the operations read). The band on screen is what Delete will hit.
    fSelClips.clear();
    Invalidate();
}

void TimelineView::ClearTimeRange() {
    if (!fRangeValid) return;
    fRangeValid = false;
    fRangeA = fRangeB = 0;
    Invalidate();
}

// Both selections gone in one gesture: what a click on empty lane area (the
// gap between lanes, the space below the last one) means. One invalidate.
void TimelineView::ClearSelectionAndRange() {
    if (fSelClips.empty() && !fRangeValid) return;
    fSelClips.clear();
    fRangeValid = false;
    fRangeA = fRangeB = 0;
    Invalidate();
}

// A press on empty lane content starts a range drag. Shift keeps the previous
// rubber-band clip select (additive), which is where that gesture moved to
// when this one took the plain drag.
void TimelineView::BeginRangeDrag(BPoint where) {
    fDrag = Drag::RangeSelect;
    fRangeAnchor = Snapped(XToFrame(where.x));
    if (fRangeAnchor < 0) fRangeAnchor = 0;
    fRangeDragged = false;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}

// A click anywhere in a lane makes that track the selected one (the inspector's
// target, and where a paste goes).
void TimelineView::SetSelectedTrack(TrackId id) {
    if (id == kInvalidTrackId || fSelectedTrack == id) return;
    fSelectedTrack = id;
    if (BWindow* w = Window()) {
        BMessage sel(kMsgTrackSelected);
        sel.AddInt64("track", (int64)id);
        w->PostMessage(&sel);
    }
    Invalidate();
}

// The name a clip shows: its own when it has one, else the source file's base
// name. One definition, so the strip and any other reader cannot disagree.
std::string TimelineView::ClipNameOf(const Clip& c) const {
    if (!c.name.empty()) return c.name;
    const std::string& p = c.sourcePath;
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// The range over the lane area: a translucent blue band with solid edges, and
// a matching bar on the ruler (drawn from DrawRuler). It is under the playhead
// and over the loop/punch tints, so the one the user just dragged reads first.
void TimelineView::DrawTimeRange() {
    if (!fRangeValid || fRangeB <= fRangeA) return;
    float x0 = FrameToX(fRangeA);
    float x1 = FrameToX(fRangeB);
    if (x1 < HeaderWidth() || x0 > Bounds().right) return;
    if (x0 < HeaderWidth()) x0 = HeaderWidth();
    const float botY = Bounds().bottom - fBarThickness;
    SetDrawingMode(B_OP_ALPHA);
    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
    SetHighColor(90, 140, 230, 45);
    FillRect(BRect(x0, ContentTop(), x1, botY));
    SetDrawingMode(B_OP_COPY);
    SetHighColor(PanelAccent(Rgb(120, 170, 255)));
    StrokeLine(BPoint(x0, ContentTop()), BPoint(x0, botY));
    StrokeLine(BPoint(x1, ContentTop()), BPoint(x1, botY));
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

// --- range operations (M2.4) ----------------------------------------------
//
// Cut/Copy/Delete/Insert Silence act on the time range; Delete/Copy also fall
// back to the clip selection when no range is set, so the Edit menu's items
// mean something in both selection modes. The model work lives in
// RangeEdit.h + the two range commands; this is the wiring.
//
// Which of the two selections an operation acts on is `SelectionTargetFor`
// (model/ArrangeLogic.h) — the view keeps them mutually exclusive, so the
// answer is always the thing the user touched last (package 50, item C:
// Delete used to take a stale range after the user had clicked a clip).
// Copies the range's content, or the selected clips when there is no range.

void TimelineView::RangeCopy() {
    switch (SelectionTargetFor(HasTimeRange(), !fSelClips.empty())) {
        case SelectTarget::Range: {
            std::vector<RangeItem> got = ExtractRange(*fProject, fRangeA, fRangeB);
            fClipboard.clear();
            for (RangeItem& it : got) {
                ClipItem ci;
                ci.midi = it.midi;
                ci.track = it.track;
                ci.audio = it.audio;
                ci.region = it.region;
                fClipboard.push_back(ci);
            }
            if (!fClipboard.empty())
                if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
            break;
        }
        case SelectTarget::Clips:
            SetClipboardToSelection();
            break;
        default:
            break;
    }
    Invalidate();
}

void TimelineView::RangeDelete() {
    if (SelectionTargetFor(HasTimeRange(), !fSelClips.empty())
        == SelectTarget::Range) {
        if (fStack->Execute(std::make_unique<RangeDeleteCommand>(fRangeA, fRangeB),
                            *fProject))
            if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
        Invalidate();
        return;
    }
    DeleteSelection();
}

void TimelineView::RangeCut() {
    switch (SelectionTargetFor(HasTimeRange(), !fSelClips.empty())) {
        case SelectTarget::Range:
            RangeCopy();
            RangeDelete();
            break;
        case SelectTarget::Clips:
            SetClipboardToSelection();
            DeleteSelection();
            break;
        default:
            break;
    }
}

void TimelineView::RangeInsertSilence() {
    if (!HasTimeRange()) return;
    if (fStack->Execute(
            std::make_unique<RangeInsertSilenceCommand>(fRangeA, fRangeB), *fProject))
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    Invalidate();
}

void TimelineView::SelectAllClips() {
    // An explicit clip gesture: it drops the time range, so Delete right after
    // it acts on the clips the user just asked for (item C).
    ClearTimeRange();
    fSelClips.clear();
    for (const Track& t : fProject->Tracks()) {
        for (const Clip& c : t.clips)
            if (!(c.takeGroup > 0 && !c.takeActive)) fSelClips.insert(c.id);
        for (const MidiClip& c : t.midiClips)
            if (!(c.takeGroup > 0 && !c.takeActive)) fSelClips.insert(c.id);
    }
    Invalidate();
}

// Nudge the selection one grid step (`,` left, `.` right) as one undo step.
// The step is the snap grid's, at each clip's own position (a tempo change
// moves it), and Off nudges by the historical 1/16 so a nudge always moves by
// something the user can see.
void TimelineView::NudgeSelection(int dir) {
    if (dir == 0 || fSelClips.empty()) return;
    auto macro = std::make_unique<MacroCommand>(dir < 0 ? "Nudge Left"
                                                        : "Nudge Right");
    for (ClipId id : fSelClips) {
        Track* t = TrackOfClip(id);
        if (!t) continue;
        if (Clip* c = t->FindClip(id)) {
            Frame ns = c->startFrame
                     + dir * SnapStep(fProject->tempoMap, c->startFrame, fSnap);
            if (ns < 0) ns = 0;
            if (ns != c->startFrame)
                macro->Add(std::make_unique<MoveClipCommand>(t->id, id, ns));
        } else if (MidiClip* m = t->FindMidiClip(id)) {
            Frame ns = m->startFrame
                     + dir * SnapStep(fProject->tempoMap, m->startFrame, fSnap);
            if (ns < 0) ns = 0;
            if (ns != m->startFrame)
                macro->Add(std::make_unique<MoveMidiClipCommand>(t->id, id, ns));
        }
    }
    if (macro->Empty()) return;
    if (fStack->Execute(std::move(macro), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgReloadEngine);
    }
}

// --- clip depth actions (M2.5), shared by the context menu and the tests ----

void TimelineView::ToggleClipMute(TrackId track, ClipId clip, bool midi) {
    Track* t = fProject->FindTrack(track);
    if (!t) return;
    bool nowMuted = true;
    if (midi) {
        MidiClip* c = t->FindMidiClip(clip);
        if (!c) return;
        nowMuted = !c->muted;
    } else {
        Clip* c = t->FindClip(clip);
        if (!c) return;
        nowMuted = !c->muted;
    }
    if (fStack->Execute(std::make_unique<SetClipMuteCommand>(
                            track, clip, midi, nowMuted), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) {
            w->PostMessage(kMsgUiRefresh);
            w->PostMessage(kMsgReloadEngine);   // the clip's stream is baked
        }
    }
}

void TimelineView::NextClipColour(TrackId track, ClipId clip) {
    Track* t = fProject->FindTrack(track);
    if (!t) return;
    Clip* c = t->FindClip(clip);
    if (!c) return;
    if (fStack->Execute(std::make_unique<SetClipColorCommand>(
                            track, clip, (c->colorIndex + 1) % kTrackColorCount),
                        *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::SetClipFadeShapeAt(TrackId track, ClipId clip, FadeShape shape) {
    if (fStack->Execute(std::make_unique<SetClipFadeShapeCommand>(track, clip, shape),
                        *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::DuplicateSelection() {
    // No clip selection but a range? Duplicate the range's content at its end,
    // which is what the same command on a time selection means.
    if (fSelClips.empty() && HasTimeRange()) {
        std::vector<RangeItem> got = ExtractRange(*fProject, fRangeA, fRangeB);
        if (got.empty()) return;
        std::vector<ClipItem> items;
        for (RangeItem& it : got) {
            ClipItem ci;
            ci.midi = it.midi;
            ci.track = it.track;
            ci.audio = it.audio;
            ci.region = it.region;
            items.push_back(ci);
        }
        PasteItems(items, fRangeB, /*toSourceTracks=*/true);
        return;
    }
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
    if (fDrag == Drag::Pencil && fDragLane >= 0) {   // the region being drawn
        const BRect lane = LaneRect(fDragLane);
        const Frame end  = Snapped(XToFrame(fHoverPos.x));
        const Frame a    = std::min(fPencilStart, end);
        const Frame b    = std::max(fPencilStart, end);
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
        SetHighColor(120, 200, 160, 90);
        FillRect(BRect(FrameToX(a), lane.top + Themed(3), FrameToX(b),
                       lane.bottom - Themed(3)));
        SetDrawingMode(B_OP_COPY);
        SetHighColor(ColMidiAccent());
        StrokeRect(BRect(FrameToX(a), lane.top + Themed(3), FrameToX(b),
                         lane.bottom - Themed(3)));
    }
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
    const float botY = Bounds().bottom - fBarThickness;
    SetDrawingMode(B_OP_ALPHA);
    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
    auto band = [&](Frame a, Frame b, uint8 r, uint8 g, uint8 bl, uint8 al) {
        float x0 = FrameToX(a), x1 = FrameToX(b);
        if (x0 < HeaderWidth()) x0 = HeaderWidth();
        if (x1 <= x0) return;
        SetHighColor(r, g, bl, al);
        FillRect(BRect(x0, ContentTop(), x1, botY));
    };
    if (tr.loopEnabled && tr.loopEnd > tr.loopStart)
        band(tr.loopStart, tr.loopEnd, 70, 120, 95, 40);      // green
    if (tr.punchEnabled && tr.punchOut > tr.punchIn)
        band(tr.punchIn, tr.punchOut, 160, 60, 60, 45);       // red
    for (const Marker& mk : fProject->markers) {
        const float x = FrameToX(mk.frame);
        if (x < HeaderWidth() || x > Bounds().right) continue;
        SetHighColor(52, 199, 89, 70);
        StrokeLine(BPoint(x, ContentTop()), BPoint(x, botY));
    }
    SetDrawingMode(B_OP_COPY);

    DrawTimeRange();         // the M2.4 range, over the tints, under the playhead
    DrawPlayhead();          // over lanes, under the ruler
    DrawMarkerLane();        // the M2.7 strip, between the ruler and the lanes
    DrawRuler(updateRect);   // ruler last so it sits above lane content
    DrawToolbar();           // the tool strip sits above the ruler
    SyncScrollBars();        // ranges follow the content, zoom and pane size
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
    Invalidate(BRect(xOld - 1, ContentTop(), xOld + 1, b.bottom));
    Invalidate(BRect(xNew - 1, ContentTop(), xNew + 1, b.bottom));
}

int TimelineView::TrackIndexAt(BPoint where) const {
    if (!fProject || where.y < ContentTop())
        return -1;
    // Walk cumulative lane heights (variable per track), scroll-offset.
    float y = ContentTop() - fScrollY;
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
                         tsub->AddItem(new ThemedMenuItem(s, NULL)); }
    menu->AddItem(tsub);
    BMenu* msub = new BMenu("Meter change");
    const char* mets[] = { "4/4", "3/4", "2/4", "6/8", "5/4", "7/8" };
    for (const char* mm : mets) msub->AddItem(new ThemedMenuItem(mm, NULL));
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
        rampItem = new ThemedMenuItem("Ramp to next tempo", NULL);
        rampItem->SetMarked(segRamp);
        menu->AddItem(rampItem);
    }

    if (nearTempo >= 0 || nearMeter >= 0) {
        menu->AddSeparatorItem();
        menu->AddItem(new ThemedMenuItem("Remove change here", NULL));
    }

    // Markers.
    const Marker* nearMarker = MarkerAt(where);
    menu->AddSeparatorItem();
    menu->AddItem(new ThemedMenuItem("Add Marker", NULL));
    if (nearMarker) {
        menu->AddItem(new ThemedMenuItem("Rename Marker" B_UTF8_ELLIPSIS, NULL));
        menu->AddItem(new ThemedMenuItem("Delete Marker", NULL));
    }

    // The loop and the punch (package 50, item A). A bare ruler click used to
    // clear the loop (and Ctrl-click the punch), which contradicted the Loop
    // button that owns that state; clearing is an explicit entry of its own,
    // disabled while there is nothing to clear.
    const Transport& rtr = fProject->transport;
    menu->AddSeparatorItem();
    BMenuItem* clearLoop = new ThemedMenuItem("Clear Loop", NULL);
    clearLoop->SetEnabled(rtr.loopEnabled || rtr.loopEnd > rtr.loopStart);
    menu->AddItem(clearLoop);
    BMenuItem* clearPunch = new ThemedMenuItem("Clear Punch", NULL);
    clearPunch->SetEnabled(rtr.punchEnabled || rtr.punchOut > rtr.punchIn);
    menu->AddItem(clearPunch);

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
            AddMarkerAt(at);
        } else if (nearMarker && label
                   && std::strcmp(label, "Delete Marker") == 0) {
            RemoveMarkerAt(nearMarker->frame, nearMarker->name);
        } else if (nearMarker && label
                   && std::strncmp(label, "Rename Marker", 13) == 0) {
            RenameMarkerAt(nearMarker->frame, nearMarker->name, where);
        } else if (label && std::strcmp(label, "Clear Loop") == 0) {
            Transport& tr = fProject->transport;
            tr.loopEnabled = false;
            tr.loopStart = tr.loopEnd = 0;
            if (BWindow* w = Window()) w->PostMessage(kMsgTransportRange);
        } else if (label && std::strcmp(label, "Clear Punch") == 0) {
            // No message: the punch is the recorder's and this view's only
            // (the transport bar's Punch lamp is re-read by the 60 Hz pulse).
            Transport& tr = fProject->transport;
            tr.punchEnabled = false;
            tr.punchIn = tr.punchOut = 0;
        }
        Invalidate();
    }
    delete menu;
}

// The marker rename prompt, shared by the ruler's menu and the marker lane's
// (package 50 made the flag's right-click a menu of its own). RenameWindow
// echoes kMsgRenameMarker with the frame in its "track" field.
void TimelineView::RenameMarkerAt(Frame frame, const std::string& name,
                                  BPoint where) {
    BPoint sp = ConvertToScreen(where);
    BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
    (new RenameWindow(wr, (TrackId)frame, name.c_str(), BMessenger(Window()),
                      kMsgRenameMarker))->Show();
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
    BMessage* cur = Window() ? Window()->CurrentMessage() : nullptr;
    if (cur) cur->FindInt32("buttons", &buttons);
    const bool rightClick = (buttons & B_SECONDARY_MOUSE_BUTTON) != 0;
    // The event's modifiers (not the live keyboard): a synthetic event from the
    // functional test can set them, which is how the range drag is driven.
    const uint32 mods = EventModifiers(cur);

    // The tool strip (M2.3): tool buttons, the snap field, the zoom pair.
    if (where.y < ToolbarHeight()) {
        if (!rightClick) HandleToolbarClick(where);
        return;
    }

    // An open in-place rename (M2.6) commits on a press outside its own field;
    // the press then does whatever it would have done. A clip's field lives in
    // its name strip (package 50), so the "inside" rect is whose field is open.
    if (fRenameTrack != kInvalidTrackId) {
        BRect field;
        bool haveField = false;
        if (fRenameClip != kInvalidClipId) {
            field = ClipRenameFieldRect();     // the clip's name strip
            haveField = field.IsValid();
        } else {
            const int rIdx = fProject->IndexOfTrack(fRenameTrack);
            if (rIdx >= 0) { field = NameRect(LaneRect(rIdx)); haveField = true; }
        }
        // No rect to compare against means the field cannot be where the press
        // is: commit, the way a press outside always does.
        if (!haveField || !field.Contains(where)) CommitRename();
    }

    // The marker lane (M2.7), between the ruler and the lanes when it is on.
    if (fMarkerLane && where.y >= RulerBottom() && where.y < ContentTop()
        && where.x >= HeaderWidth()) {
        HandleMarkerLaneClick(where, rightClick);
        return;
    }

    // Right-click on the ruler: tempo / meter change menu.
    if (rightClick && where.y >= ToolbarHeight() && where.y < RulerBottom()
        && where.x >= HeaderWidth()) {
        HandleRulerMenu(where);
        return;
    }

    // Ruler: click seeks, drag sets a loop region. Ctrl-drag sets the punch
    // range, Alt-drag the time range (M2.4). On release we decide
    // seek-vs-drag by how far it moved.
    if (where.y >= ToolbarHeight() && where.y < RulerBottom()
        && where.x >= HeaderWidth()) {
        // Left-click a marker flag: jump the playhead there.
        if (!rightClick) {
            if (const Marker* mk = MarkerAt(where)) {
                SeekTo(mk->frame);
                return;
            }
        }
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        // The time range's modifier: Alt. Accepting BOTH modifier bits is
        // deliberate — Haiku maps the PC's Alt key to B_COMMAND_KEY and the
        // Option/Windows key to B_OPTION_KEY, and this codebase's existing
        // "Alt-drag" (the slip gesture) tests for B_OPTION_KEY; a user's Alt
        // must make a range either way.
        if ((mods & (B_OPTION_KEY | B_COMMAND_KEY)) != 0) {
            fDrag = Drag::RangeSelect;
            fRangeAnchor = f;
            fRangeDragged = false;
        } else {
            fDrag        = (mods & B_CONTROL_KEY) ? Drag::RulerPunch
                                                  : Drag::RulerLoop;
            fLoopAnchor  = f;
            fLoopDragged = false;
        }
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }

    // The lane's resize edge (M2.6) sits in the header gutter. Checked before
    // the lane itself: the gain fader's last row of pixels is inside the grab
    // band, and a press there must still mean "fader".
    if (where.x < HeaderWidth() && !rightClick) {
        const int edge = LaneEdgeAt(where);
        if (edge >= 0 && HeaderControlAt(edge, where) == HeaderControl::None
            && edge < (int)fProject->Tracks().size()) {
            const Track& et = fProject->Tracks()[(size_t)edge];
            fDrag       = Drag::LaneResize;
            fDragTrack  = et.id;
            fDragLane   = edge;
            fDragOrig   = (float)et.height;    // the pre-drag value, for undo
            fDragGrabY  = where.y;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
            return;
        }
    }

    const int idx = TrackIndexAt(where);
    if (idx < 0) {
        // The empty space of the lane area: the 1 px gap between two lanes,
        // and everything below the last lane. It used to be swallowed whole —
        // not even clearing the selection, so the next Delete acted on a clip
        // the user was no longer pointing at. Now it is lane area like any
        // other: a left click clears both selections, a right click gets the
        // lane menu (there is no track under the cursor, so its Paste goes to
        // the playhead through the ordinary paste-target rule).
        const float laneAreaBottom = Bounds().bottom - fBarThickness;
        if (where.y >= ContentTop() && where.y <= laneAreaBottom
            && where.x >= 0.0f) {
            if (rightClick) {
                LaneAreaMenu(where, kInvalidTrackId);
            } else {
                ClearSelectionAndRange();
            }
        }
        return;
    }
    BRect lane = LaneRect(idx);
    const Track& t = fProject->Tracks()[idx];

    if (where.x < HeaderWidth()) {
        // A click on the lane's meter clears its clip lamp (the master meter's
        // own gesture, package 52) and then carries on: the click still selects
        // the track, which is what it has always done.
        if (where.x >= HeaderWidth() - HdrMeterW() && !rightClick)
            ClearClipLamps(t.id);
        // Any header click selects the track (drives the left inspector).
        SetSelectedTrack(t.id);
        // Track name strip: right-click deletes the track, double-click renames
        // it IN PLACE (M2.6; "Rename…" in this menu still opens the window).
        if (where.y <= lane.top + Themed(18) && where.x < 114
            && HeaderControlAt(idx, where) == HeaderControl::None) {
            if (rightClick) {
                BPopUpMenu* mm = new BPopUpMenu("trk", false, false);
                mm->AddItem(new ThemedMenuItem("Move Up", NULL));
                mm->AddItem(new ThemedMenuItem("Move Down", NULL));
                mm->AddItem(new ThemedMenuItem("Next Color", NULL));
                mm->AddItem(new ThemedMenuItem("Taller", NULL));
                mm->AddItem(new ThemedMenuItem("Shorter", NULL));
                mm->AddSeparatorItem();
                BMenuItem* ssItem = new ThemedMenuItem("Solo Safe", NULL);
                ssItem->SetMarked(t.soloSafe);
                mm->AddItem(ssItem);
                // Mute-group submenu: muting any member mutes the whole group.
                BMenu* mg = new BMenu("Mute Group");
                BMenuItem* mgNone = new ThemedMenuItem("None", NULL);
                mgNone->SetMarked(t.muteGroup == 0);
                mg->AddItem(mgNone);
                for (int g = 1; g <= 4; g++) {
                    char lb[16];
                    std::snprintf(lb, sizeof(lb), "Group %d", g);
                    BMenuItem* gi = new ThemedMenuItem(lb, NULL);
                    gi->SetMarked(t.muteGroup == g);
                    mg->AddItem(gi);
                }
                mm->AddItem(mg);
                mm->AddSeparatorItem();
                mm->AddItem(new ThemedMenuItem("Duplicate Track", NULL));
                mm->AddItem(new ThemedMenuItem(t.collapsed ? "Expand Lane"
                                                           : "Collapse Lane", NULL));
                mm->AddSeparatorItem();
                mm->AddItem(new ThemedMenuItem(t.frozen ? "Unfreeze" : "Freeze", NULL));
                mm->AddItem(new ThemedMenuItem("Rename" B_UTF8_ELLIPSIS, NULL));
                mm->AddItem(new ThemedMenuItem("Delete", NULL));
                BMenuItem* sel = mm->Go(ConvertToScreen(where), false, true);
                const std::string pick = sel ? std::string(sel->Label())
                                             : std::string();
                delete mm;
                // Color/height/reorder route through the command stack (undoable).
                if (pick == "Move Up" || pick == "Move Down") {
                    fStack->Execute(std::make_unique<MoveTrackCommand>(
                                        t.id, pick == "Move Up" ? -1 : +1),
                                    *fProject);
                    // A reorder is a track-LIST edit: the console draws the
                    // arrangement's order, so an open mixer has to be told or it
                    // keeps showing the old one (package 52).
                    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
                } else if (pick == "Next Color") {
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
                } else if (pick == "Duplicate Track") {
                    DuplicateTrack(t.id);
                } else if (pick == "Collapse Lane" || pick == "Expand Lane") {
                    SetTrackCollapsed(t.id, pick == "Collapse Lane");
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
                } else if (pick == "Delete") {
                    fStack->Execute(std::make_unique<RemoveTrackCommand>(t.id),
                                    *fProject);
                    // The console's strip for this track has to go with it
                    // (package 52): left up it looks live and drops every edit
                    // on the FindTrack guard, silently.
                    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
                }
                Invalidate();
                return;
            }
            int32 clicks = 1;
            if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
                m->FindInt32("clicks", &clicks);
            if (clicks >= 2) {   // double-click a track name: rename IN PLACE
                BeginTrackRename(t.id);
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

    // The non-pointer tools (M2.3): each owns its click in the content area.
    // The pointer tool falls through to the behaviour below, which is the
    // arrangement editor's original one. A right-click keeps doing what it
    // always did (the delete/paste menus) whichever tool is selected.
    if (!rightClick && fTool != Tool::Pointer) {
        const Hit hit = HitTest(where);
        switch (fTool) {
            case Tool::Scissors: ScissorsAt(hit, where); return;
            case Tool::Glue:     GlueAt(hit);            return;
            case Tool::Mute:     MuteAt(hit);            return;
            case Tool::Fade:     FadeStart(hit, where);   return;
            case Tool::Pencil:
                if (PencilAt(hit, where)) return;
                break;    // on an existing region: the pointer's move/resize
            default: break;
        }
    }

    // MIDI track content: regions behave like audio clips. Double-click opens
    // the piano roll (creating a region first on empty space); drag moves or
    // (right edge) resizes; right-click gets the SAME block menu an audio clip
    // does (package 50: it was a three-item stub). Notes are edited in the
    // piano roll, not here.
    if (t.type == TrackType::Midi) {
        const Frame at = XToFrame(where.x);
        // The region on top at `at` — the draw-order rule, so a region that
        // overlaps another (a comp, a crossfade) is the one the click acts on.
        auto hiddenMidi = [](const MidiClip& c) {
            return c.takeGroup > 0 && !c.takeActive;
        };
        const int hit = TopmostClipIndexAt(t.midiClips, at, hiddenMidi);
        int32 clicks = 1;
        if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
            m->FindInt32("clicks", &clicks);

        if (rightClick) {
            if (hit >= 0) {
                const MidiClip& c = t.midiClips[(size_t)hit];
                const std::string pick =
                    ClipMenu(where, /*midi=*/true, /*withTake=*/c.takeGroup > 0);
                if (pick == "Copy") {
                    SetClipboardToRegion(c);
                } else if (pick == "Delete") {
                    fStack->Execute(std::make_unique<RemoveMidiClipCommand>(
                        t.id, c.id), *fProject);
                } else if (pick == "Split here") {
                    Frame sat = Snapped(XToFrame(where.x));
                    fStack->Execute(std::make_unique<SplitMidiClipCommand>(
                        t.id, c.id, sat), *fProject);
                } else if (pick == "Mute Clip" || pick == "Unmute Clip") {
                    ToggleClipMute(t.id, c.id, /*midi=*/true);
                } else if (pick == "Clear Fades") {
                    fStack->Execute(std::make_unique<SetMidiClipFadeCommand>(
                        t.id, c.id, 0, 0), *fProject);
                } else if (pick == "Next Take" && c.takeGroup > 0) {
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
            } else {
                // Empty lane: the lane area's menu, which offers Paste (onto
                // this track) among the rest.
                LaneAreaMenu(where, t.id);
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
            // Selecting a block drops the time range: the two selections are
            // mutually exclusive, newest gesture wins (package 50, item C).
            ClearTimeRange();
            if (mods & B_SHIFT_KEY) {
                if (ClipSelected(c.id)) fSelClips.erase(c.id);
                else                    fSelClips.insert(c.id);
                Invalidate(lane);
                return;
            }
            if (!ClipSelected(c.id)) { fSelClips.clear(); fSelClips.insert(c.id); }
            SetSelectedTrack(t.id);
            fDragTrack       = t.id;
            fDragLane        = idx;
            fDragClip        = c.id;
            fDragClipOrig    = c.startFrame;
            fDragClipOrigLen = c.lengthFrames;
            fDragClipSrcOrig = 0;              // MIDI regions have no source
            fDragFadeInOrig  = c.fadeInFrames;
            fDragFadeOutOrig = c.fadeOutFrames;
            fDragIsMidiClip  = true;
            // Which part of the region was grabbed comes from the same
            // hit-test the hover highlight and the cursor use.
            const Zone zone = HitTest(where).zone;
            if (zone == Zone::FadeIn) {
                fDrag = Drag::ClipFadeIn;    // top-left grip = fade in
            } else if (zone == Zone::FadeOut) {
                fDrag = Drag::ClipFadeOut;   // top-right grip = fade out
            } else if (zone == Zone::TrimRight) {
                fDrag = Drag::ClipResize;
            } else if (zone == Zone::TrimLeft) {
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
        // Empty lane: a plain click clears the selection (and a drag makes a
        // time range, M2.4); Shift keeps the additive rubber-band select.
        // The two are mutually exclusive (item C): a band select drops the
        // range, the range drag drops the clip selection.
        SetSelectedTrack(t.id);
        if (!rightClick) {
            if (mods & B_SHIFT_KEY) {
                ClearTimeRange();          // clip gesture: the range goes
                fBanding = true;
                fBandA = fBandB = where;
                Invalidate(lane);
            } else {
                if (!fSelClips.empty()) { fSelClips.clear(); Invalidate(lane); }
                BeginRangeDrag(where);
            }
        }
        return;
    }

    // Content area: right-click opens the block's menu; otherwise start
    // dragging it. Which block: the one DRAWN LAST of those under the pointer
    // (TopmostClipIndexAt) — the first match was the clip UNDERNEATH a
    // crossfade, which is not the one the user sees on top.
    const Frame at = XToFrame(where.x);
    auto hidden = [](const Clip& c) { return c.takeGroup > 0 && !c.takeActive; };
    const int clipIdx = TopmostClipIndexAt(t.clips, at, hidden);
    if (clipIdx >= 0) {
        const Clip& c = t.clips[(size_t)clipIdx];
        if (rightClick) {
            const std::string pick =
                ClipMenu(where, /*midi=*/false, /*withTake=*/c.takeGroup > 0);
            if (pick == "Copy") {
                SetClipboardToClip(c);
            } else if (pick == "Rename Clip" B_UTF8_ELLIPSIS) {
                BeginClipRename(t.id, c.id);   // in place, in the name strip
            } else if (pick == "Next Colour") {
                NextClipColour(t.id, c.id);
            } else if (pick == "Mute Clip" || pick == "Unmute Clip") {
                ToggleClipMute(t.id, c.id, /*midi=*/false);
            } else if (pick.rfind("Fade ", 0) == 0) {
                FadeShape shape = pick == "Fade Equal Power"
                                      ? FadeShape::EqualPower
                                  : pick == "Fade S-Curve" ? FadeShape::SCurve
                                                           : FadeShape::Linear;
                SetClipFadeShapeAt(t.id, c.id, shape);
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
        // Double-click an audio clip: rename it (M2.5) — IN PLACE, in its
        // own name strip (package 50; it used to open RenameWindow at the
        // raw click point, which can hang off the screen). The MIDI path's
        // double-click opens the piano roll instead — a region's content is
        // what you edit there, a clip's name is what you edit here.
        int32 clicks = 1;
        if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
            m->FindInt32("clicks", &clicks);
        if (clicks >= 2) {
            BeginClipRename(t.id, c.id);
            return;
        }
        // Selection: Shift-click toggles; a plain click on an unselected
        // clip selects only it (a click on an already-selected clip keeps
        // the whole selection so it can be dragged as a group).
        // Selecting a block drops the time range: the two selections are
        // mutually exclusive, newest gesture wins (package 50, item C) —
        // this is what stops Delete from acting on a stale range.
        ClearTimeRange();
        if (mods & B_SHIFT_KEY) {
            if (ClipSelected(c.id)) fSelClips.erase(c.id);
            else                    fSelClips.insert(c.id);
            Invalidate(lane);
            return;
        }
        if (!ClipSelected(c.id)) {
            fSelClips.clear();
            fSelClips.insert(c.id);
        }
        SetSelectedTrack(t.id);
        fMultiMove       = false;
        fDragTrack       = t.id;
        fDragLane        = idx;
        fDragClip        = c.id;
        fDragClipOrig    = c.startFrame;
        fDragClipOrigLen = c.lengthFrames;
        fDragClipSrcOrig = c.sourceOffset;
        fDragFadeInOrig  = c.fadeInFrames;
        fDragFadeOutOrig = c.fadeOutFrames;
        // The grabbed part, from the one hit-test everything shares.
        const Zone zone = HitTest(where).zone;
        if (zone == Zone::Gain) {
            fDrag = Drag::ClipGain;        // Ctrl-drag vertical = clip gain
            fDragOrig = c.gain;
        } else if (zone == Zone::Slip) {
            fDrag = Drag::Slip;            // Alt-drag = slip the audio
            fDragSlipOrig  = c.sourceOffset;
            fDragSlipGrabX = where.x;
        } else if (zone == Zone::FadeIn) {
            fDrag = Drag::ClipFadeIn;      // top-left corner = fade in
        } else if (zone == Zone::FadeOut) {
            fDrag = Drag::ClipFadeOut;     // top-right corner = fade out
        } else if (zone == Zone::TrimRight
                   && (mods & (B_COMMAND_KEY | B_OPTION_KEY))) {
            // Cmd/Alt on the right edge stretches the clip to fit instead of
            // trimming it: the audio plays slower or faster and the clip
            // covers the length the drag gives it (M7.2).
            fDrag = Drag::ClipStretch;
            fDragRatioOrig = c.stretchRatio;
        } else if (zone == Zone::TrimRight) {
            fDrag = Drag::ClipResize;
        } else if (zone == Zone::TrimLeft) {
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
        return;
    }

    // The empty part of an audio lane: the lane area's menu (paste included,
    // disabled without a clipboard) and a click that clears both selections.
    if (fDrag == Drag::None && rightClick) {
        LaneAreaMenu(where, t.id);
        return;
    }

    // Left-click on empty audio content: the track becomes selected, and a
    // drag makes a time range (M2.4). Shift keeps the additive rubber-band
    // clip select -- the gesture the plain drag used to be. A band select
    // drops the time range and vice versa: newest gesture wins (item C).
    if (fDrag == Drag::None && !rightClick && where.x >= HeaderWidth()) {
        SetSelectedTrack(t.id);
        if (mods & B_SHIFT_KEY) {
            ClearTimeRange();          // clip gesture: the range goes
            fBanding = true;
            fBandA = fBandB = where;
            SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        } else {
            if (!fSelClips.empty()) fSelClips.clear();
            BeginRangeDrag(where);
        }
        Invalidate();
    }
}

void TimelineView::HandleHeaderClick(const Track& t, BRect lane, BPoint where) {
    const TrackId id = t.id;
    const HeaderControl ctl = HeaderControlAt(fProject->IndexOfTrack(id), where);
    // The modifier the event carried: Alt-clicking Solo is exclusive solo
    // (M2.6). Both Alt bits are accepted, like the range drag (Haiku maps the
    // PC's Alt to B_COMMAND_KEY and Option to B_OPTION_KEY).
    const uint32 mods = EventModifiers(Window() ? Window()->CurrentMessage()
                                                : nullptr);

    if (ctl == HeaderControl::Fold) {   // fold the lane to its title strip
        ToggleTrackCollapsed(id);
        return;
    }
    if (ctl == HeaderControl::Colour) {
        OpenColourPicker(id, where);
        return;
    }
    if (ctl == HeaderControl::Input) {
        OpenInputPicker(id, where);
        return;
    }

    // Mute / solo: immediate toggle commands.
    if (ctl == HeaderControl::Mute) {
        fStack->Execute(std::make_unique<SetTrackMuteCommand>(id, !t.muted),
                        *fProject);
        Invalidate(lane);
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
        return;
    }
    if (ctl == HeaderControl::Solo) {
        if ((mods & (B_OPTION_KEY | B_COMMAND_KEY)) != 0) {
            ExclusiveSolo(id);   // Alt-click: this track alone
        } else {
            fStack->Execute(std::make_unique<SetTrackSoloCommand>(id, !t.soloed),
                            *fProject);
        }
        Invalidate(lane);
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
        return;
    }
    if (ctl == HeaderControl::Monitor) {
        // Per-track input monitor: hear this track's input live without arming.
        if (Track* tr = fProject->FindTrack(id)) tr->inputMonitor = !tr->inputMonitor;
        Invalidate(lane);
        if (BWindow* w = Window()) {
            w->PostMessage(kMsgMonitorRefresh);
            w->PostMessage(kMsgUiRefresh);
        }
        return;
    }
    if (ctl == HeaderControl::Arm) {
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
    if (ctl == HeaderControl::Gain)     mode = Drag::Gain;
    else if (ctl == HeaderControl::Pan) mode = Drag::Pan;
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

// --- track depth actions (M2.6) --------------------------------------------
//
// The header buttons, the context menu and the Track menu all run these, and
// so does the functional test: a popup menu Go()s synchronously, so the suite
// drives the method the menu entry calls (the pattern M2.3/M2.5 established)
// and the drawn control is what the shots are for.

void TimelineView::SetTrackColour(TrackId track, int colorIndex) {
    if (!fProject || !fStack) return;
    if (!fProject->FindTrack(track)) return;
    if (colorIndex < 0) colorIndex = 0;
    if (fStack->Execute(std::make_unique<SetTrackColorCommand>(track, colorIndex),
                        *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::SetTrackInput(TrackId track, InputSource input) {
    if (!fProject || !fStack) return;
    const Track* t = fProject->FindTrack(track);
    if (!t) return;
    // A channel choice carries its width too (M5): naming one channel means a
    // mono take, naming a pair stereo. One gesture, two undoable edits.
    const TrackChannels width = WidthOfChoice(input);
    const bool setWidth = (width != t->channels);
    if (fStack->Execute(std::make_unique<SetTrackInputCommand>(track,
                                                              std::move(input)),
                        *fProject)) {
        if (setWidth)
            fStack->Execute(std::make_unique<SetTrackChannelsCommand>(track, width),
                            *fProject);
        Invalidate();
        // A changed input re-evaluates idle monitoring (the same message the
        // inspector's picker posts), and repaints the panes.
        if (BWindow* w = Window()) {
            w->PostMessage(kMsgMonitorRefresh);
            w->PostMessage(kMsgUiRefresh);
        }
    }
}

// Folding the lane that owns the open name field would hide the field (a
// collapsed lane draws no clips and its header is the title strip): the field
// would keep the keys while showing nothing. Commit it first, the way a press
// outside does — the text the user typed is not lost. (Package 50.)
void TimelineView::SetTrackCollapsed(TrackId track, bool collapsed) {
    if (!fProject || !fStack) return;
    const Track* t = fProject->FindTrack(track);
    if (!t || t->collapsed == collapsed) return;
    if (collapsed && fRenameTrack == track) CommitRename();
    if (fStack->Execute(std::make_unique<SetTrackCollapsedCommand>(track,
                                                                  collapsed),
                        *fProject)) {
        SyncScrollBars();   // the content height changed
        // Keep the lane the user just folded or expanded in view: shrinking
        // the content makes the vertical scrollbar clamp, which shifts every
        // lane on screen, and the one that was clicked can end up under the
        // bottom edge.
        const int idx = fProject->IndexOfTrack(track);
        if (idx >= 0) {
            const BRect lane = LaneRect(idx);
            const float limit = Bounds().bottom - fBarThickness;
            if (lane.bottom > limit)
                ScrollToY(fScrollY + (lane.bottom - limit));
        }
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::ToggleTrackCollapsed(TrackId track) {
    const Track* t = fProject ? fProject->FindTrack(track) : nullptr;
    if (t) SetTrackCollapsed(track, !t->collapsed);
}

// Alt-click on a Solo button: this track alone. One MacroCommand over the
// tracks that actually change, so it is a single "Exclusive Solo" undo step;
// Alt-clicking the track that is already the only soloed one clears it (the
// same toggle every other solo button has).
void TimelineView::ExclusiveSolo(TrackId track) {
    if (!fProject || !fStack) return;
    const Track* t = fProject->FindTrack(track);
    if (!t) return;
    bool onlyThis = t->soloed;
    for (const Track& o : fProject->Tracks())
        if (o.id != track && o.soloed) { onlyThis = false; break; }
    const bool want = !onlyThis;

    auto macro = std::make_unique<MacroCommand>("Exclusive Solo");
    for (const Track& o : fProject->Tracks()) {
        const bool target = (o.id == track) ? want : false;
        if (o.soloed != target)
            macro->Add(std::make_unique<SetTrackSoloCommand>(o.id, target));
    }
    if (macro->Empty()) return;
    if (fStack->Execute(std::move(macro), *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

// Duplicate a whole track. The copy is selected and scrolled into view: the
// user asked for it, and a new lane somewhere below the fold would otherwise
// look like nothing happened.
void TimelineView::DuplicateTrack(TrackId track) {
    if (!fProject || !fStack) return;
    if (!fProject->FindTrack(track)) return;
    auto cmd = std::make_unique<DuplicateTrackCommand>(track);
    DuplicateTrackCommand* cp = cmd.get();
    if (!fStack->Execute(std::move(cmd), *fProject)) return;

    const TrackId newId = cp->CreatedId();
    SetSelectedTrack(newId);          // posts kMsgTrackSelected (inspector)
    const int idx = fProject->IndexOfTrack(newId);
    if (idx >= 0) {
        const BRect lane = LaneRect(idx);      // with the CURRENT scroll
        const float limit = Bounds().bottom - fBarThickness;
        if (lane.bottom > limit)
            ScrollToY(fScrollY + (lane.bottom - limit));
    }
    Invalidate();
    if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
}

void TimelineView::DuplicateSelectedTrack() {
    if (fSelectedTrack != kInvalidTrackId) DuplicateTrack(fSelectedTrack);
}

// The colour picker: the palette is TrackColor (one definition), the labels
// come from beside it, and the chip is drawn on the item itself.
namespace {
class ColourMenuItem : public ThemedMenuItem {
public:
    ColourMenuItem(int index, rgb_color chip)
        : ThemedMenuItem(TrackColourName(index), nullptr), fChip(chip) {}
    void DrawContent() override {
        BMenu* m = Menu();
        if (m == nullptr) return;
        // ContentLocation() is where the stock item starts its label, and
        // GetContentSize() its line height -- so the chip and the label land on
        // the same line as every other menu entry without guessing.
        const BPoint at = ContentLocation();
        float w = 0.0f, h = 0.0f;
        GetContentSize(&w, &h);
        BFont font;
        m->GetFont(&font);
        font_height fh;
        font.GetHeight(&fh);
        const float side = std::min(h - Themed(2.0f), Themed(11.0f));
        const float top = at.y + (h - side) * 0.5f;
        BRect chip(at.x, top, at.x + side, top + side);
        m->SetHighColor(fChip);
        m->FillRoundRect(chip, Themed(2.0f), Themed(2.0f));
        m->SetHighColor(ColBtnBorder());
        m->StrokeRoundRect(chip, Themed(2.0f), Themed(2.0f));
        m->SetHighColor(IsEnabled() ? ColText() : ColTextDim());
        m->DrawString(Label(), BPoint(chip.right + Themed(6.0f),
                                      at.y + fh.ascent));
    }
private:
    rgb_color fChip;
};
} // namespace

void TimelineView::OpenColourPicker(TrackId track, BPoint where) {
    const Track* t = fProject ? fProject->FindTrack(track) : nullptr;
    if (!t) return;
    BPopUpMenu* menu = new BPopUpMenu("track colour", false, false);
    for (int i = 0; i < kTrackColorCount; i++) {
        BMenuItem* it = new ColourMenuItem(i, TrackColor(i));
        if (i == t->colorIndex) it->SetMarked(true);
        menu->AddItem(it);
    }
    BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
    const int pick = sel ? menu->IndexOf(sel) : -1;
    delete menu;
    if (pick >= 0) SetTrackColour(track, pick);
}

void TimelineView::OpenInputPicker(TrackId track, BPoint where) {
    const Track* t = fProject ? fProject->FindTrack(track) : nullptr;
    if (!t) return;
    const std::vector<InputSource> choices = TrackInputChoices(*t);
    BPopUpMenu* menu = new BPopUpMenu("record input", false, false);
    for (size_t i = 0; i < choices.size(); i++) {
        BMenuItem* it = new ThemedMenuItem(
            InputChoiceLabel(choices[i]).c_str(), nullptr);
        if (SameInput(choices[i], t->input))
            it->SetMarked(true);
        menu->AddItem(it);
    }
    BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
    const int pick = sel ? menu->IndexOf(sel) : -1;
    delete menu;
    if (pick >= 0 && (size_t)pick < choices.size())
        SetTrackInput(track, choices[(size_t)pick]);
}

// --- rename in place (M2.6) ------------------------------------------------

void TimelineView::BeginTrackRename(TrackId track) {
    const Track* t = fProject ? fProject->FindTrack(track) : nullptr;
    if (!t) return;
    fRenameTrack = track;
    fRenameClip  = kInvalidClipId;
    fRenameText  = t->name;
    fRenameCaret = (int)fRenameText.size();
    MakeFocus(true);              // the keys are this view's while it is open
    Invalidate();
}

// The name strip inside a clip's block: what DrawClip paints and what the
// in-place rename field covers. One definition, so the drawn strip, the field
// and the "did the press land outside the field" test cannot disagree.
static BRect ClipStripOf(const BRect& block) {
    const float stripH = Themed(15.0f);
    return BRect(block.left + Themed(1), block.top + Themed(1),
                 block.right - Themed(1), block.top + stripH);
}
// A clip's block, the way DrawClip lays it out.
static BRect ClipBlockOf(const BRect& lane, float x0, float x1) {
    return BRect(x0, lane.top + Themed(3), x1, lane.bottom - Themed(3));
}

// The clip whose name field is open: its name strip, in view coordinates. An
// invalid rect when no clip rename is open (or the clip is gone).
BRect TimelineView::ClipRenameFieldRect() const {
    if (fRenameClip == kInvalidClipId) return BRect();
    const Track* t = fProject ? fProject->FindTrack(fRenameTrack) : nullptr;
    const Clip*  c = t ? t->FindClip(fRenameClip) : nullptr;
    if (!c) return BRect();
    const int idx = fProject->IndexOfTrack(fRenameTrack);
    if (idx < 0) return BRect();
    float x0 = FrameToX(c->startFrame);
    const float x1 = FrameToX(c->startFrame + c->lengthFrames);
    if (x0 < HeaderWidth()) x0 = HeaderWidth();
    if (x1 <= x0) return BRect();
    return ClipStripOf(ClipBlockOf(LaneRect(idx), x0, x1));
}

// Rename a clip IN PLACE, in its own name strip (package 50): a double-click
// on the clip (or the menu's "Rename Clip…") used to open RenameWindow at the
// raw click point, which can land half off the screen. The field machinery is
// the track rename's — one buffer, one caret, the same keys — because the two
// are the same edit with a different target.
//
// A clip too narrow to hold a field falls back to the window (which is now
// clamped to the screen by RenameWindow itself): there is nowhere for a caret
// to live on a strip that is 20 px wide.
void TimelineView::BeginClipRename(TrackId track, ClipId clip) {
    const Track* t = fProject ? fProject->FindTrack(track) : nullptr;
    const Clip*  c = t ? t->FindClip(clip) : nullptr;
    if (!c) return;
    const float w = FrameToX(c->startFrame + c->lengthFrames)
                  - FrameToX(c->startFrame);
    if (w < Themed(48.0f)) {
        const int idx = fProject->IndexOfTrack(track);
        if (idx < 0) return;
        float x0 = FrameToX(c->startFrame);
        const float x1 = FrameToX(c->startFrame + c->lengthFrames);
        if (x0 < HeaderWidth()) x0 = HeaderWidth();
        const BRect strip = ClipStripOf(ClipBlockOf(LaneRect(idx), x0, x1));
        BPoint sp = ConvertToScreen(BPoint(strip.left, strip.top));
        BRect wr(sp.x, sp.y, sp.x + 260, sp.y + 74);
        (new RenameWindow(wr, track, ClipNameOf(*c).c_str(), BMessenger(this),
                          kMsgRenameClip, (int64)clip, "Rename Clip"))->Show();
        return;
    }
    fRenameTrack = track;
    fRenameClip  = clip;
    fRenameText  = ClipNameOf(*c);
    fRenameCaret = (int)fRenameText.size();
    MakeFocus(true);
    Invalidate();
}

void TimelineView::CancelTrackRename() {
    if (fRenameTrack == kInvalidTrackId && fRenameText.empty()) return;
    fRenameTrack = kInvalidTrackId;
    fRenameClip  = kInvalidClipId;
    fRenameText.clear();
    fRenameCaret = 0;
    Invalidate();
}

// Enter (or a press outside the field): one command, whichever kind of name
// this is. An empty or unchanged text commits nothing, as before.
void TimelineView::CommitRename() {
    const TrackId id     = fRenameTrack;
    const ClipId  clipId = fRenameClip;
    const std::string text = fRenameText;
    fRenameTrack = kInvalidTrackId;
    fRenameClip  = kInvalidClipId;
    fRenameText.clear();
    fRenameCaret = 0;
    if (id != kInvalidTrackId && !text.empty()) {
        bool changed = false;
        std::unique_ptr<Command> cmd;
        if (clipId != kInvalidClipId) {
            Track* t = fProject->FindTrack(id);
            const Clip* c = t ? t->FindClip(clipId) : nullptr;
            if (c && text != ClipNameOf(*c)) {
                changed = true;
                cmd = std::make_unique<SetClipNameCommand>(id, clipId, text);
            }
        } else if (Track* t = fProject->FindTrack(id); t && text != t->name) {
            changed = true;
            cmd = std::make_unique<SetTrackNameCommand>(id, text);
        }
        if (changed && fStack->Execute(std::move(cmd), *fProject))
            if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
    Invalidate();
}

// The keys the open name field owns. While it is open EVERY key is consumed:
// a bare `,` or `1` would otherwise nudge a clip or switch tools behind the
// field, which is how a rename edits the wrong thing.
bool TimelineView::HandleRenameKey(const char* bytes, int32 numBytes) {
    if (fRenameTrack == kInvalidTrackId || numBytes < 1) return false;
    const unsigned char c = (unsigned char)bytes[0];
    switch (c) {
        case B_RETURN:               // (== B_ENTER on this platform) commits
            CommitRename();
            return true;
        case B_ESCAPE:               CancelTrackRename();  return true;
        case B_BACKSPACE:
            if (fRenameCaret > 0) {
                fRenameText.erase((size_t)fRenameCaret - 1, 1);
                --fRenameCaret;
            }
            Invalidate();
            return true;
        case B_LEFT_ARROW:
            if (fRenameCaret > 0) --fRenameCaret;
            Invalidate();
            return true;
        case B_RIGHT_ARROW:
            if (fRenameCaret < (int)fRenameText.size()) ++fRenameCaret;
            Invalidate();
            return true;
        case B_DELETE:
            if (fRenameCaret < (int)fRenameText.size())
                fRenameText.erase((size_t)fRenameCaret, 1);
            Invalidate();
            return true;
        default: break;
    }
    if (c >= 0x20 && c != 0x7f) {                 // a printable ASCII byte
        fRenameText.insert((size_t)fRenameCaret, 1, (char)c);
        ++fRenameCaret;
        Invalidate();
    }
    return true;
}

// Compute the dragged value from the cursor x and write it straight into the
// model for live feedback. This is a transient preview only; the undoable
// command is pushed in MouseUp.
void TimelineView::PreviewDrag(BPoint where) {
    // A range drag (M2.4), from the ruler or from empty lane content. Same
    // click-vs-drag threshold as the loop: a press that never moves is a
    // click, and a click must not leave a sliver of a range behind.
    if (fDrag == Drag::RangeSelect) {
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        const Frame lo = std::min(fRangeAnchor, f);
        const Frame hi = std::max(fRangeAnchor, f);
        const Frame minDrag = (Frame)(4.0 * fFramesPerPixel);   // ~4 px
        if (hi - lo <= minDrag) return;   // not a real drag yet
        fRangeDragged = true;
        SetTimeRange(lo, hi);
        return;
    }

    // A lane-edge drag (M2.6): the pointer's distance from the grab point is
    // the height delta, previewed straight into the model (one command on
    // release, like every other drag here). The clamp matches the drag's own
    // floor and SetTrackHeightCommand's ceiling.
    if (fDrag == Drag::LaneResize) {
        Track* t = fProject->FindTrack(fDragTrack);
        if (!t) return;
        float h = fDragOrig + (where.y - fDragGrabY);
        if (h < Themed(72.0f))  h = Themed(72.0f);
        if (h > Themed(300.0f)) h = Themed(300.0f);
        t->height = (int)(h + 0.5f);
        Invalidate();
        return;
    }

    // A marker-flag drag (package 50): no track either. The model is NOT
    // touched — the flag's new frame is previewed as a line on the lane and
    // committed as one command on release, so a drag is one undo step.
    if (fDrag == Drag::Marker) {
        Frame f = Snapped(XToFrame(where.x));
        if (f < 0) f = 0;
        // The same ~4 px threshold the ruler drags below use: a click wobbles a
        // pixel or two during button-down, and without it that wobble turned
        // the click into a move -- the flag jumped a grid step and the seek the
        // click was meant to be never happened. Under the threshold the flag
        // stays where it was and the release is still a click.
        if (!fDragMarkerMoved) {
            const float dx = where.x - fDragMarkerDown.x;
            const float dy = where.y - fDragMarkerDown.y;
            if (dx * dx + dy * dy < 16.0f) return;   // 4 px
            fDragMarkerMoved = true;
        }
        if (f != fDragMarkerTo) {
            fDragMarkerTo = f;
            Invalidate(BRect(0, RulerBottom(), Bounds().right, ContentTop()));
        }
        return;
    }

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
        Invalidate(BRect(0, ToolbarHeight(), Bounds().right, ContentTop()));
        return;
    }

    Track* t = fProject->FindTrack(fDragTrack);
    if (!t) return;
    BRect lane = LaneRect(fDragLane);

    if (fDrag == Drag::Gain || fDrag == Drag::Pan) {
        // The header fader and pan knob are the ONE preview whose value other
        // panes also show: the inspector draws it and the mixer holds a
        // snapshot of it, so each step has to reach them or they sit on the
        // old value until the mouse comes up (the mixer/inspector -> timeline
        // direction has always been live: InspectorView::Refresh() posts this
        // same message on every move). Same message, same cadence, no second
        // mechanism -- the undo discipline is untouched (still preview writes,
        // one command on mouse-up).
        if (fDrag == Drag::Gain) {
            BRect g = GainRect(lane);
            float v = (where.x - g.left) / g.Width() * kMaxGain;
            if (v < 0) v = 0; if (v > kMaxGain) v = kMaxGain;
            t->gain = v;
        } else {
            // Pan knob: vertical drag (up = right), relative to the grab point.
            float v = fDragOrig + (fDragGrabY - where.y) / 100.0f;
            if (v < -1) v = -1; if (v > 1) v = 1;
            t->pan = v;
        }
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
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
    } else if (fDrag == Drag::ClipStretch) {
        // Cmd/Alt right-edge drag: the clip covers the dragged timeline length
        // and its material is stretched to fill it, so the audio keeps its
        // pitch and the clip fits the bars.
        Clip* c = t->FindClip(fDragClip);
        if (c && !fDragIsMidiClip) {
            Frame len = Snapped(XToFrame(where.x)) - c->startFrame;
            if (len < 1) len = 1;
            // The ratio comes from the span the clip referenced when the drag
            // STARTED (the pre-drag length and ratio), never from the file's
            // length and never from the preview the last motion event wrote:
            // a clip is a window into its file, and a trimmed loop must stretch
            // the material it shows.
            Clip base = *c;
            base.lengthFrames = fDragClipOrigLen;
            base.stretchRatio = fDragRatioOrig;
            // The source's own length (from the peak cache, which already knows
            // it) clamps the span to the material that exists; 0 = unknown, and
            // then the derived ratio simply stands.
            Frame fileFrames = 0;
            double srcRate = fProject ? fProject->sampleRate : 48000.0;
            if (fPeaks) {
                auto it = fPeaks->find(c->sourcePath);
                if (it != fPeaks->end() && it->second.IsValid()) {
                    fileFrames = it->second.TotalFrames();
                    if (it->second.SampleRate() > 0.0f)
                        srcRate = it->second.SampleRate();
                }
            }
            c->lengthFrames = len;
            c->stretchRatio = StretchRatioForDrag(
                base, len, fileFrames, srcRate,
                fProject ? fProject->sampleRate : 48000.0);
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
    } else if (fDrag == Drag::Slip) {
        Clip* c = t->FindClip(fDragClip);
        if (c) {
            // Dragging right pulls LATER material under the clip's window (the
            // audio is grabbed and moved, the clip stays). Not snapped: this is
            // an offset into the file, not a musical position.
            Frame off = fDragSlipOrig
                      + (Frame)((where.x - fDragSlipGrabX) * fFramesPerPixel);
            if (off < 0) off = 0;
            c->sourceOffset = off;
        }
    }
    Invalidate(lane);
}

void TimelineView::MouseMoved(BPoint where, uint32 transit, const BMessage* drag) {
    (void)drag;
    if (fDrag != Drag::None || fBanding || fAutoDragging)
        fHoverValid = false;          // a gesture's preview owns the screen
    else
        UpdateHover(where, 0, transit);
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
    if (fDrag == Drag::Pencil) {   // the drawn region grows with the pointer
        fHoverPos = where;
        if (fDragLane >= 0) Invalidate(LaneRect(fDragLane));
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

    // Pencil: the drag ends by creating the region it previewed. A bare click
    // (no drag) makes one bar, the same default a double-click uses.
    if (fDrag == Drag::Pencil) {
        fPencilLen = std::max(fPencilStart, Snapped(XToFrame(where.x)))
                   - std::min(fPencilStart, Snapped(XToFrame(where.x)));
        fPencilStart = std::min(fPencilStart, Snapped(XToFrame(where.x)));
        CommitPencil(where);
        fDrag = Drag::None;
        fDragLane = -1;
        fHoverPos = where;
        return;
    }

    // A marker-flag drag (package 50): released where it was grabbed it is the
    // old click-to-seek; anywhere else the marker MOVES — one snapped,
    // undoable step (MoveMarkerCommand), committed here so the whole gesture
    // is one undo entry.
    if (fDrag == Drag::Marker) {
        const Frame       orig = fDragMarkerOrig;
        const Frame       to   = fDragMarkerTo;
        const std::string name = fDragMarkerName;
        fDrag = Drag::None;
        fDragMarkerName.clear();
        if (to == orig) SeekTo(orig);
        else            MoveMarkerTo(orig, name, to);
        Invalidate();
        return;
    }

    // Range drag: a real drag left a range behind; a bare click (no movement)
    // clears the range, the way clicking empty lane content always cleared the
    // clip selection.
    if (fDrag == Drag::RangeSelect) {
        if (!fRangeDragged) ClearTimeRange();
        fDrag = Drag::None;
        Invalidate();
        return;
    }

    // A lane-edge drag (M2.6): restore the pre-drag height, then push ONE
    // command carrying the dragged result.
    if (fDrag == Drag::LaneResize) {
        if (Track* t = fProject->FindTrack(fDragTrack)) {
            const int v = t->height;
            t->height = (int)(fDragOrig + 0.5f);
            if (v != t->height)
                fStack->Execute(std::make_unique<SetTrackHeightCommand>(
                                    fDragTrack, v), *fProject);
            SyncScrollBars();   // the content height changed
            if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
        }
        fDrag = Drag::None;
        Invalidate();
        return;
    }

    // Ruler: a real drag leaves a loop region; a bare click is a SEEK and
    // nothing else (package 50, item A). It used to clear the loop — so a
    // plain click silently switched off a loop the user had set, in direct
    // contradiction of the Loop button that owns that state. Clearing has its
    // own affordance now: the ruler menu's "Clear Loop".
    if (fDrag == Drag::RulerLoop) {
        Transport& tr = fProject->transport;
        if (!fLoopDragged) {
            tr.playhead = fLoopAnchor;
            SetPlayhead(fLoopAnchor);
            if (BWindow* w = Window())
                w->PostMessage(kMsgSeek);
        } else if (BWindow* w = Window()) {
            // The drag committed a range: the running graph walks the range it
            // was built with (M4.2), so this re-positions the engine — once,
            // here, not on every step of the drag.
            w->PostMessage(kMsgTransportRange);
        }
        fDrag = Drag::None;
        Invalidate();
        return;
    }
    // Punch drag: a bare Ctrl-click seeks too — same rule, and the punch is
    // cleared from the ruler menu's "Clear Punch".
    if (fDrag == Drag::RulerPunch) {
        if (!fLoopDragged) {
            fProject->transport.playhead = fLoopAnchor;
            SetPlayhead(fLoopAnchor);
            if (BWindow* w = Window()) w->PostMessage(kMsgSeek);
        }
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
        } else if (fDrag == Drag::ClipStretch) {
            // Restore the pre-drag geometry AND ratio, then commit the whole
            // gesture as one Stretch Clip step (a drag is one clean undo).
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame  v = c->lengthFrames;
                const double r = c->stretchRatio;
                c->lengthFrames = fDragClipOrigLen;
                c->stretchRatio = fDragRatioOrig;
                if (v != fDragClipOrigLen
                    || std::fabs(r - fDragRatioOrig) > kStretchEpsilon)
                    cmd = std::make_unique<SetClipStretchCommand>(
                            fDragTrack, fDragClip, v, r, c->pitchSemis);
            }
        } else if (fDrag == Drag::Slip) {
            // Restore the pre-drag offset, then commit the whole gesture as one
            // SlipClipCommand carrying the dragged value.
            if (Clip* c = t->FindClip(fDragClip)) {
                const Frame v = c->sourceOffset;
                c->sourceOffset = fDragSlipOrig;
                if (v != fDragSlipOrig)
                    cmd = std::make_unique<SlipClipCommand>(fDragTrack, fDragClip, v);
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
        fLiveTake.clear();
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

void TimelineView::DrawLiveAudio(BRect region, TrackId track) {
    // The take that BELONGS to this lane: one capture writes one file per
    // armed track (M5), so drawing take 0 on every lane would show another
    // track's channels here.
    const auto ti = fLiveTake.find(track);
    if (ti == fLiveTake.end() || ti->second < 0) return;
    const size_t take = (size_t)ti->second;
    const size_t count = fLiveRec->EnvCount(take);
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
            const float mn = fLiveRec->EnvMin(take, j);
            const float mx = fLiveRec->EnvMax(take, j);
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
    StrokeLine(BPoint(x, ContentTop()), BPoint(x, Bounds().bottom));
}

void TimelineView::DrawRuler(BRect update) {
    BRect r = Bounds();
    r.top    = ToolbarHeight();
    r.bottom = RulerBottom();   // the marker lane (when on) is below this

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
            FillRect(BRect(lx0, r.top, lx1, r.bottom));
        }
    }
    // Punch region (Ctrl-drag): a red band on the lower half of the ruler.
    if (tr.punchEnabled && tr.punchOut > tr.punchIn) {
        float px0 = FrameToX(tr.punchIn);
        float px1 = FrameToX(tr.punchOut);
        if (px0 < HeaderWidth()) px0 = HeaderWidth();
        if (px1 > px0) {
            SetHighColor(Rgb(150, 60, 60));
            FillRect(BRect(px0, r.bottom - Themed(6), px1, r.bottom));
        }
    }

    // The time range (M2.4): a band along the ruler's top and both edges, so
    // the range reads even when every lane is scrolled out of the way.
    if (fRangeValid && fRangeB > fRangeA) {
        float x0 = FrameToX(fRangeA), x1 = FrameToX(fRangeB);
        if (x0 < HeaderWidth()) x0 = HeaderWidth();
        if (x1 > x0) {
            SetHighColor(PanelAccent(Rgb(120, 170, 255)));
            FillRect(BRect(x0, r.top, x1, r.top + Themed(3)));
            StrokeLine(BPoint(x0, r.top), BPoint(x0, r.bottom));
            StrokeLine(BPoint(x1, r.top), BPoint(x1, r.bottom));
        }
    }

    // Bar/beat ticks: bars full-height + numbered, beats short (when zoomed in).
    ForEachGridLine([&](float x, bool isBar, long bar) {
        SetHighColor(isBar ? ColText() : ColGrid());
        StrokeLine(BPoint(x, isBar ? r.top : r.bottom - Themed(8)),
                   BPoint(x, r.bottom));
        if (isBar) {
            char label[16];
            std::snprintf(label, sizeof(label), "%ld", bar);
            DrawString(label, BPoint(x + Themed(3), r.bottom - Themed(9)));
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
                SetHighColor(PanelAccent(Rgb(230, 170, 70)));
                StrokeLine(BPoint(x0, up ? r.bottom - Themed(3) : r.top + Themed(3)),
                           BPoint(x1, up ? r.top + Themed(3) : r.bottom - Themed(3)));
            }
        }
        if (!onScreen) continue;
        SetHighColor(PanelAccent(Rgb(230, 170, 70)));
        StrokeLine(BPoint(x, r.top), BPoint(x, r.bottom));
        char s[16];
        std::snprintf(s, sizeof(s), t.ramp ? "%.0f~" : "%.0f", t.bpm);
        DrawString(s, BPoint(x + Themed(2), r.top + Themed(9)));
    }
    for (const MeterChange& m : tm.Meters()) {
        const float x = FrameToX(m.frame);
        if (x < HeaderWidth() || x > r.right) continue;
        SetHighColor(PanelAccent(Rgb(120, 190, 230)));
        char s[16]; std::snprintf(s, sizeof(s), "%d/%d", m.num, m.denom);
        DrawString(s, BPoint(x + Themed(2), r.top + Themed(19)));
    }

    // Position markers: a small flag + name at the top of the ruler. The name
    // is truncated to the NEXT flag (or the view's right edge) — the lane below
    // truncates the same string, and an untruncated one here ran straight
    // through the following marker's label (package 50, item J1).
    for (std::size_t i = 0; i < fProject->markers.size(); i++) {
        const Marker& mk = fProject->markers[i];
        const float x = FrameToX(mk.frame);
        if (x < HeaderWidth() || x > r.right) continue;
        SetHighColor(ColMidiAccent());   // green flag, distinct from tempo/meter
        FillRect(BRect(x, r.top, x + Themed(8), r.top + Themed(7)));
        StrokeLine(BPoint(x, r.top), BPoint(x, r.bottom));
        // Room to the next marker's flag (markers are kept sorted by frame), or
        // to the view's edge — whichever comes first.
        float room = r.right;
        if (i + 1 < fProject->markers.size()) {
            const float nx = FrameToX(fProject->markers[i + 1].frame);
            if (nx < room) room = nx - Themed(6.0f);
        }
        room -= x + Themed(10.0f);
        if (room > Themed(12.0f)) {
            BString name(mk.name.c_str());
            TruncateString(&name, B_TRUNCATE_END, room);
            SetHighColor(ColText());
            DrawString(name.String(), BPoint(x + Themed(10), r.top + Themed(8)));
        }
    }
}

// --- markers (M2.7) ---------------------------------------------------------

// The one seek: the model's playhead, the DRAWN line (the ruler's flag click
// used to move the model without moving the line, which left the screen
// showing the old position while stopped), and the engine/intents a seek
// posts.
void TimelineView::SeekTo(Frame frame) {
    if (!fProject) return;
    if (frame < 0) frame = 0;
    fProject->transport.playhead = frame;
    SetPlayhead(frame);
    if (BWindow* w = Window()) w->PostMessage(kMsgSeek);
}

void TimelineView::AddMarkerAt(Frame frame) {
    if (!fProject || !fStack) return;
    if (frame < 0) frame = 0;
    char nm[32];
    std::snprintf(nm, sizeof(nm), "Marker %d",
                  (int)fProject->markers.size() + 1);
    if (fStack->Execute(std::make_unique<AddMarkerCommand>(frame, nm), *fProject)) {
        Invalidate();
        // The markers window (and the other panes) follow through this one
        // message -- an open list that does not show a new marker is stale
        // content.
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::RemoveMarkerAt(Frame frame, const std::string& name) {
    if (!fProject || !fStack) return;
    if (fStack->Execute(std::make_unique<RemoveMarkerCommand>(frame, name),
                        *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

void TimelineView::SetMarkerLane(bool on) {
    if (fMarkerLane == on) return;
    fMarkerLane = on;
    // The lanes start lower now: the scroll ranges and the vertical clamp both
    // read ContentTop(), so both have to be recomputed.
    SyncScrollBars();
    Invalidate();
}

// The marker whose flag or drawn name is under a point in the marker lane.
const Marker* TimelineView::MarkerInLane(BPoint where) const {
    if (!fProject) return nullptr;
    if (where.y < RulerBottom() || where.y >= ContentTop()) return nullptr;
    for (const Marker& mk : fProject->markers) {
        const float x = FrameToX(mk.frame);
        // The hit zone is the flag PLUS the name to its right, so clicking the
        // label works too (the ruler's flag-only 9 px zone is part of why this
        // lane exists). Names that overlap: the first (leftmost) wins, which is
        // the one whose flag the pointer is nearest.
        BString nm(mk.name.c_str());
        const float w = std::min(StringWidth(nm), Themed(96.0f));
        if (where.x >= x - Themed(4.0f) && where.x <= x + Themed(11.0f) + w)
            return &mk;
    }
    return nullptr;
}

void TimelineView::HandleMarkerLaneClick(BPoint where, bool rightClick) {
    int32 clicks = 1;
    if (BMessage* m = Window() ? Window()->CurrentMessage() : nullptr)
        m->FindInt32("clicks", &clicks);
    const Marker* mk = MarkerInLane(where);
    if (rightClick) {
        // A flag's right-click opens a menu of its own now (package 50): it
        // used to delete the marker outright — one keystroke's worth of
        // damage with no confirmation. Empty lane space still adds a marker.
        if (mk) {
            const Frame       frame = mk->frame;
            const std::string name  = mk->name;
            BPopUpMenu* menu = new BPopUpMenu("mk", false, false);
            menu->AddItem(new ThemedMenuItem("Rename Marker" B_UTF8_ELLIPSIS, NULL));
            menu->AddItem(new ThemedMenuItem("Delete Marker", NULL));
            BMenuItem* sel = menu->Go(ConvertToScreen(where), false, true);
            const std::string pick = sel ? std::string(sel->Label())
                                         : std::string();
            delete menu;
            if (pick.rfind("Rename Marker", 0) == 0)
                RenameMarkerAt(frame, name, where);
            else if (pick == "Delete Marker")
                RemoveMarkerAt(frame, name);
        } else {
            AddMarkerAt(Snapped(XToFrame(where.x)));
        }
        return;
    }
    if (mk) {
        // A press on a flag starts a possible DRAG: released where it was
        // grabbed, it is the click-to-seek it always was; released anywhere
        // else it MOVES the marker (snapped, one undo step, committed on
        // release — the model is not touched while the flag is in flight).
        fDrag           = Drag::Marker;
        fDragMarkerOrig = mk->frame;
        fDragMarkerTo   = mk->frame;
        fDragMarkerName = mk->name;
        fDragMarkerDown = where;
        fDragMarkerMoved = false;
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
        return;
    }
    if (clicks >= 2) AddMarkerAt(Snapped(XToFrame(where.x)));
}

// The marker lane's drag, through the stack: one MoveMarkerCommand, one undo
// step. Public like the clip-depth actions, so the functional test drives
// exactly what the drag and the flag's menu run.
void TimelineView::MoveMarkerTo(Frame oldFrame, const std::string& name,
                                Frame newFrame) {
    if (!fProject || !fStack) return;
    if (newFrame < 0) newFrame = 0;
    if (fStack->Execute(
            std::make_unique<MoveMarkerCommand>(oldFrame, name, newFrame),
            *fProject)) {
        Invalidate();
        if (BWindow* w = Window()) w->PostMessage(kMsgUiRefresh);
    }
}

// The lane itself: a header-coloured strip with the flags and their names.
void TimelineView::DrawMarkerLane() {
    if (!fMarkerLane || !fProject) return;
    const float top = RulerBottom();
    const float bot = ContentTop() - 1.0f;
    if (bot <= top) return;
    SetHighColor(ColHeader());
    FillRect(BRect(0, top, Bounds().right, bot));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(0, bot), BPoint(Bounds().right, bot));
    // The gutter names the strip (an unlabelled band is an "unexplained
    // empty band", which the guidelines call a bug).
    SetHighColor(ColTextDim());
    DrawString("Markers", BPoint(Themed(10.0f), top + Themed(14.0f)));

    for (const Marker& mk : fProject->markers) {
        const float x = FrameToX(mk.frame);
        if (x < HeaderWidth() - Themed(4.0f) || x > Bounds().right) continue;
        SetHighColor(ColMidiAccent());
        BPoint flag[3] = { BPoint(x, top + Themed(3.0f)),
                           BPoint(x + Themed(9.0f), top + Themed(7.0f)),
                           BPoint(x, top + Themed(11.0f)) };
        FillPolygon(flag, 3);
        StrokeLine(BPoint(x, top + Themed(3.0f)), BPoint(x, bot - Themed(2.0f)));
        BString name(mk.name.c_str());
        TruncateString(&name, B_TRUNCATE_END, Themed(96.0f));
        SetHighColor(ColText());
        DrawString(name.String(), BPoint(x + Themed(12.0f), top + Themed(14.0f)));
    }
    // The playhead crosses the strip, so a marker jump is visible here too.
    const float px = FrameToX(fPlayhead);
    if (px >= HeaderWidth() && px <= Bounds().right) {
        SetHighColor(ColPlayhead());
        StrokeLine(BPoint(px, top), BPoint(px, bot));
    }

    // A flag being dragged: the line the release will land on (the model is
    // untouched until then).
    if (fDrag == Drag::Marker) {
        const float dx = FrameToX(fDragMarkerTo);
        if (dx >= HeaderWidth() && dx <= Bounds().right) {
            SetHighColor(ColAccent());
            StrokeLine(BPoint(dx, top), BPoint(dx, bot));
        }
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
    if (target >= 0) SeekTo(target);
}

// Shift-L: loop between the markers bracketing the playhead. Returns whether
// it set a range (true) — ToggleLoop uses the answer to decide whether it
// still needs a default range (package 50, item D).
bool TimelineView::LoopBetweenMarkers() {
    const Frame ph = fProject->transport.playhead;
    Frame lo = -1, hi = -1;
    for (const Marker& m : fProject->markers) {
        if (m.frame <= ph) lo = m.frame;
        if (m.frame > ph)  { hi = m.frame; break; }
    }
    if (lo < 0 || hi < 0 || hi <= lo) return false;   // need a bracketing pair
    Transport& tr = fProject->transport;
    tr.loopStart = lo; tr.loopEnd = hi; tr.loopEnabled = true;
    Invalidate();
    // The running graph walks a range it was built with (M4.2): re-position so
    // the new one is what plays.
    if (BWindow* w = Window()) w->PostMessage(kMsgTransportRange);
    return true;
}

// The marker whose flag is under `where` on the ruler, or nullptr.
const Marker* TimelineView::MarkerAt(BPoint where) const {
    if (!fProject || where.y < ToolbarHeight() || where.y >= RulerBottom())
        return nullptr;
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
    // A folded lane is the title strip, whatever its unfolded height is (M2.6).
    if (t.collapsed) return CollapsedTrackHeight();
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
    float top = ContentTop() - fScrollY;
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

    // An empty project: the lane area says how to fill it (UI_GUIDELINES §3,
    // "an empty pane says how to fill it"). Before this it was a blank void —
    // the Empty template makes exactly this state, so it is the FIRST thing a
    // new user sees.
    if (fProject->Tracks().empty()) {
        const float top = ContentTop();
        const float bot = Bounds().bottom - fBarThickness;
        if (bot > top) {
            SetHighColor(ColLane());
            FillRect(BRect(0, top, Bounds().right, bot));
            const char* hint = "Drag audio or MIDI files here";
            const char* hint2 = "or use Track \xE2\x96\xB8 New Audio Track";
            const float cx = (HeaderWidth() + Bounds().right - fBarThickness) * 0.5f;
            const float cy = (top + bot) * 0.5f;
            SetHighColor(ColText());
            DrawString(hint, BPoint(cx - StringWidth(hint) * 0.5f,
                                    cy - Themed(8.0f)));
            // The sub-line is a touch smaller, and the font is PUT BACK: the
            // view keeps one font between draws, so leaving the small one on
            // would shrink everything drawn after it.
            BFont keep;
            GetFont(&keep);
            BFont small(be_plain_font);
            small.SetSize(Themed(10.5f));
            SetFont(&small);
            SetHighColor(ColTextDim());
            DrawString(hint2, BPoint(cx - StringWidth(hint2) * 0.5f,
                                     cy + Themed(10.0f)));
            SetFont(&keep);
            SetHighColor(ColText());
        }
        return;
    }

    int idx = 0;
    for (const Track& t : fProject->Tracks()) {
        BRect lane = LaneRect(idx);
        // Off-screen lanes cost nothing: the draw is handed the update rect
        // precisely so it can skip them (idx still advances -- it names the
        // lane, not the row on screen).
        if (!lane.Intersects(update)) { idx++; continue; }

        SetHighColor((idx & 1) ? ColLaneAlt() : ColLane());
        FillRect(lane);

        // A folded lane (M2.6) is its title strip: no grid, no clips, no
        // waveform -- the lane says what it is and nothing else.
        if (t.collapsed) {
            DrawTrackHeaderCompact(t, lane);
            idx++;
            continue;
        }

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
            // A clip's own colour when it has one (0 = the track's, the same
            // rule MidiClip uses).
            const rgb_color base = TrackColor(c.colorIndex ? c.colorIndex
                                                           : t.colorIndex);
            DrawClip(c, lane, base, ef, update);
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
                    DrawLiveAudio(rb, t.id);
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

    // The fold triangle (M2.6): down = expanded, right = folded. Drawn before
    // the name so the name field's own fill cannot bury it.
    {
        const BRect fr = FoldRect(lane);
        SetHighColor(ColText());
        const float m = Themed(3.0f);
        if (t.collapsed) {
            BPoint p[3] = { BPoint(fr.left + m, fr.top + m),
                            BPoint(fr.left + m + Themed(6.0f), (fr.top + fr.bottom) * 0.5f),
                            BPoint(fr.left + m, fr.bottom - m) };
            FillPolygon(p, 3);
        } else {
            BPoint p[3] = { BPoint(fr.left + m, fr.top + m),
                            BPoint(fr.right - m, fr.top + m),
                            BPoint((fr.left + fr.right) * 0.5f, fr.top + m + Themed(6.0f)) };
            FillPolygon(p, 3);
        }
    }

    // Name (or the in-place rename field, M2.6), then the colour swatch.
    if (t.id == fRenameTrack)
        DrawNameField(t, lane);
    else {
        SetHighColor(ColText());
        BString nm(t.name.c_str());
        TruncateString(&nm, B_TRUNCATE_END,
                       NameRect(lane).Width() - Themed(4.0f));
        DrawString(nm.String(), BPoint(NameRect(lane).left + Themed(1.0f),
                                       lane.top + Themed(14.0f)));
    }
    // The colour swatch: the track's palette colour, in the one place the
    // picker opens from.
    {
        const BRect sw = ColourSwatchRect(lane);
        SetHighColor(TrackColor(t.colorIndex));
        FillRoundRect(sw, Themed(2.0f), Themed(2.0f));
        SetHighColor(ColBtnBorder());
        StrokeRoundRect(sw, Themed(2.0f), Themed(2.0f));
    }

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

    // The record-input selector (M2.6): a compact field showing the chosen
    // source ("Default", an endpoint's tail, or an em dash for none), lit
    // while a source is set. The full list is the popup -- one definition,
    // shared with the inspector (TrackInputs.h).
    {
        const BRect ir = InputRect(lane);
        SetHighColor(ColBtnOff());
        FillRoundRect(ir, Themed(3.0f), Themed(3.0f));
        SetHighColor(t.input.kind != InputSource::kNone ? ColMon()
                                                        : ColBtnBorder());
        StrokeRoundRect(ir, Themed(3.0f), Themed(3.0f));
        BString lab("In: ");
        lab << InputShortLabel(t.input).c_str();
        TruncateString(&lab, B_TRUNCATE_END, ir.Width() - Themed(8.0f));
        SetHighColor(t.input.kind != InputSource::kNone ? ColMon()
                                                        : ColTextDim());
        DrawString(lab.String(), BPoint(ir.left + Themed(4.0f),
                                        ir.bottom - Themed(4.0f)));
    }

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
    SetHighColor(ColLcd());
    FillRect(meterBox);
    float mPeakL = 0.0f, mPeakR = 0.0f;
    if (auto it = fTrackPeaks.find(t.id); it != fTrackPeaks.end()) {
        mPeakL = it->second.first; mPeakR = it->second.second;
    }
    // The hold + lamp state (package 52), moved here: this draw is what the
    // meter tick triggers, and PeakHold is a time-based machine (1.5 s of hold,
    // then 20 dB/s), so it has to see the clock rather than the repaint count.
    TrackMeterState& ms = fTrackMeter[t.id];
    {
        const int64_t now = system_time();
        ms.holdL.Update(LinToFrac(mPeakL), now);
        ms.holdR.Update(LinToFrac(mPeakR), now);
        ms.clipL.Latch(ReachesClipLevel(mPeakL));
        ms.clipR.Latch(ReachesClipLevel(mPeakR));
    }
    // dB-scaled, through the shared bar (M3.3): the same maths and the same
    // one threshold set as the master meter, so a level means the same thing
    // in both places. One bar per channel, with its own hold tick and a clip
    // lamp over the pair.
    const float lampH = Themed(5.0f);
    BRect lamp(meterBox.left + Themed(1), meterBox.top,
               meterBox.right - Themed(1), meterBox.top + lampH);
    DrawClipLamp(this, lamp, ms.clipL.Clipped() || ms.clipR.Clipped());
    const float barsTop = lamp.bottom + Themed(2.0f);
    const float bw = (meterBox.Width() - Themed(3)) * 0.5f;
    auto meterBar = [&](float x0, float level, float holdFrac) {
        DrawMeterBar(this, BRect(x0, barsTop, x0 + bw, meterBox.bottom),
                     level, false, -1.0f, holdFrac);
    };
    meterBar(meterBox.left + Themed(1), mPeakL, ms.holdL.Frac());
    meterBar(meterBox.left + Themed(2) + bw, mPeakR, ms.holdR.Frac());

    // Hover highlight (M2.1): the header control the pointer is over -- or the
    // lane's resize edge (M2.6). Drawn last so it is not buried under the
    // control's own fill.
    if (fHoverValid && fHover.track == t.id) {
        if (fHoverLaneEdge) {
            SetHighColor(ColAccent());
            FillRect(BRect(0, lane.bottom - Themed(2.0f), HeaderWidth() - 1,
                           lane.bottom));
        } else if (fHoverHeader != HeaderControl::None) {
            const BRect r = HeaderRectFor(fProject->IndexOfTrack(t.id),
                                          fHoverHeader);
            SetHighColor(ColAccent());
            SetPenSize(Themed(2.0f));
            if (fHoverHeader == HeaderControl::Pan) StrokeEllipse(r);
            else StrokeRoundRect(r, Themed(3.0f), Themed(3.0f));
            SetPenSize(1.0f);
        }
    }
}

// The compact row a folded lane shows (M2.6): fold triangle, colour swatch,
// name, mute, solo -- and the meter, which stays on the right edge. No clips,
// no pan/fader/input: a folded lane is a title strip.
void TimelineView::DrawTrackHeaderCompact(const Track& t, BRect lane) {
    BRect hdr(0, lane.top, HeaderWidth(), lane.bottom);
    const bool selected = (t.id == fSelectedTrack);
    SetHighColor(selected ? ColHeaderHi() : ColHeader());
    FillRect(hdr);
    SetHighColor(t.type == TrackType::Midi ? ColMidiAccent()
               : t.type == TrackType::Bus  ? ColTextDim() : ColAudioAccent());
    FillRect(BRect(0, lane.top, Themed(4), lane.bottom));
    SetHighColor(ColGrid());
    StrokeLine(BPoint(HeaderWidth() - 1, lane.top),
               BPoint(HeaderWidth() - 1, lane.bottom));
    if (selected) {
        SetHighColor(ColAccent());
        StrokeRect(BRect(Themed(1), lane.top + Themed(1),
                         HeaderWidth() - Themed(2), lane.bottom - Themed(1)));
    }

    {   // the fold triangle, pointing right when folded
        const BRect fr = FoldRectCompact(lane);
        SetHighColor(ColText());
        const float m = Themed(3.0f);
        BPoint p[3] = { BPoint(fr.left + m, fr.top + m),
                        BPoint(fr.left + m + Themed(6.0f),
                               (fr.top + fr.bottom) * 0.5f),
                        BPoint(fr.left + m, fr.bottom - m) };
        FillPolygon(p, 3);
    }
    {   // the colour swatch stays: the colour is the track's identity
        const BRect sw = ColourSwatchRectCompact(lane);
        SetHighColor(TrackColor(t.colorIndex));
        FillRoundRect(sw, Themed(2.0f), Themed(2.0f));
        SetHighColor(ColBtnBorder());
        StrokeRoundRect(sw, Themed(2.0f), Themed(2.0f));
    }
    if (t.id == fRenameTrack) {
        DrawNameField(t, lane);
    } else {
        BString nm(t.name.c_str());
        TruncateString(&nm, B_TRUNCATE_END, Themed(56.0f));
        SetHighColor(ColText());
        DrawString(nm.String(), BPoint(Themed(38.0f), lane.top + Themed(15.0f)));
    }
    DrawButton(this, MuteRectCompact(lane), "M", t.muted,  ColMute());
    DrawButton(this, SoloRectCompact(lane), "S", t.soloed, ColSolo());

    // The meter, exactly as the expanded header draws it.
    const float mx0 = HeaderWidth() - HdrMeterW();
    BRect meterBox(mx0, lane.top + Themed(2), HeaderWidth() - Themed(2),
                   lane.bottom - Themed(2));
    SetHighColor(ColLcd());
    FillRect(meterBox);
    float mPeakL = 0.0f, mPeakR = 0.0f;
    if (auto it = fTrackPeaks.find(t.id); it != fTrackPeaks.end()) {
        mPeakL = it->second.first; mPeakR = it->second.second;
    }
    const float bw = (meterBox.Width() - Themed(3)) * 0.5f;
    auto meterBar = [&](float x0, float level) {
        if (level < 0.0f) level = 0.0f; if (level > 1.0f) level = 1.0f;
        const float h = meterBox.Height() * level;
        SetHighColor(MeterColor(level));
        FillRect(BRect(x0, meterBox.bottom - h, x0 + bw, meterBox.bottom));
    };
    meterBar(meterBox.left + Themed(1), mPeakL);
    meterBar(meterBox.left + Themed(2) + bw, mPeakR);

    if (fHoverValid && fHover.track == t.id
        && fHoverHeader != HeaderControl::None) {
        const BRect r = HeaderRectFor(fProject->IndexOfTrack(t.id),
                                      fHoverHeader);
        SetHighColor(ColAccent());
        SetPenSize(Themed(2.0f));
        StrokeRoundRect(r, Themed(3.0f), Themed(3.0f));
        SetPenSize(1.0f);
    }
}

// The in-place rename's field: a sunken well over the name, with the edit
// buffer and a caret. The caret scrolls the text so it stays visible in a
// narrow field (a long name edited at the end must not draw the caret outside
// the box). One drawing for both kinds of name — a track's (in the header
// gutter) and a clip's (over its name strip, package 50); only the rect
// differs.
void TimelineView::DrawRenameFieldIn(const BRect& r) {
    SetHighColor(ColLcd());
    FillRoundRect(r, Themed(2.0f), Themed(2.0f));
    SetHighColor(ColAccent());
    StrokeRoundRect(r, Themed(2.0f), Themed(2.0f));

    const float pad = Themed(3.0f);
    const char* full = fRenameText.c_str();
    // The caret's pixel offset within the whole string, and the shift needed
    // to keep it inside the field.
    const float caretW = StringWidth(full, (int32)fRenameCaret);
    float shift = 0.0f;
    const float room = r.Width() - pad * 2.0f;
    if (caretW > room) shift = caretW - room;

    SetHighColor(ColText());
    DrawString(full, BPoint(r.left + pad - shift, r.bottom - Themed(4.0f)));
    const float cx = r.left + pad - shift + caretW;
    SetHighColor(ColText());
    StrokeLine(BPoint(cx, r.top + Themed(3.0f)), BPoint(cx, r.bottom - Themed(3.0f)));
}

void TimelineView::DrawNameField(const Track&, BRect lane) {
    DrawRenameFieldIn(NameRect(lane));
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

    SetHighColor(PanelAccent(Rgb(230, 200, 90)));
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
                            const ClipFades& ef, BRect update) {
    float x0 = FrameToX(c.startFrame);
    float x1 = FrameToX(c.startFrame + c.lengthFrames);
    if (x1 < HeaderWidth() || x0 > lane.right)
        return;                       // fully outside the content area
    // ...and nothing of it is being repainted.
    if (x1 < update.left || x0 > update.right)
        return;
    if (x0 < HeaderWidth()) x0 = HeaderWidth();

    const Frame fadeIn  = ef.fadeIn;
    const Frame fadeOut = ef.fadeOut;

    // A muted clip is drawn desaturated so it reads as silent at a glance; the
    // waveform still shows (you can still edit it), just dimmer.
    const rgb_color fill = c.muted ? TintColor(base, 0.45f) : base;

    BRect block = ClipBlockOf(lane, x0, x1);
    SetHighColor(fill);
    FillRoundRect(block, Themed(5), Themed(5));        // Logic-style rounded region

    DrawClipWave(c, block, update, ef);

    // Fade ramps: the actual CURVE (M2.5) from the block corner up to where the
    // fade ends, sampled. These are the effective fades and their shapes, so an
    // auto-crossfade's equal-power ramp draws here as the sound has it.
    SetHighColor(ColClipBorder());
    // `fromSilence`: the ramp starts silent at xFrom (a fade-in) and reaches
    // full at xTo; otherwise it starts full and reaches silence at xTo.
    auto drawRamp = [&](float xFrom, float xTo, FadeShape shape, bool fromSilence) {
        if (xTo <= xFrom) return;
        const int steps = 12;
        float px = xFrom;
        float py = block.bottom - (fromSilence ? 0.0f : 1.0f) * block.Height();
        for (int i = 1; i <= steps; i++) {
            const float s = (float)i / (float)steps;   // along the ramp
            const float t = fromSilence ? s : 1.0f - s;
            const float g = FadeGainAt(shape, t);
            const float x = xFrom + (xTo - xFrom) * s;
            const float y = block.bottom - g * block.Height();
            StrokeLine(BPoint(px, py), BPoint(x, y));
            px = x; py = y;
        }
    };
    if (fadeIn > 0) {
        const float xEnd = FrameToX(c.startFrame + fadeIn);
        if (xEnd > block.left)
            drawRamp(block.left, std::min(xEnd, block.right), ef.inShape, true);
    }
    if (fadeOut > 0) {
        const float xStart = FrameToX(c.startFrame + c.lengthFrames - fadeOut);
        if (xStart < block.right)
            drawRamp(std::max(xStart, block.left), block.right, ef.outShape, false);
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

    // Name strip across the top (darker shade of the clip's colour), like
    // Logic. The name is the clip's own (M2.5) or the file's base name, and a
    // muted clip's strip goes grey so "muted" is readable from the strip alone.
    const float stripH = Themed(15.0f);
    if (block.Height() > stripH + Themed(2) && block.Width() > Themed(10)) {
        BRect strip = ClipStripOf(block);
        const float shade = c.muted ? 0.32f : 0.5f;
        SetHighColor(Rgb((uint8)(base.red * shade), (uint8)(base.green * shade),
                         (uint8)(base.blue * shade)));
        FillRect(strip);
        if (fRenameClip == c.id) {
            // The open in-place rename field takes the strip (package 50): the
            // clip's name is edited where it is drawn, not in a window floating
            // over the arrangement.
            DrawRenameFieldIn(strip);
        } else {
        // Truncated to the strip: a name runs into the next region otherwise.
        BString name(ClipNameOf(c).c_str());
        const float room = strip.Width() - Themed(14)
                         - (c.muted ? Themed(14) : 0.0f);
        if (room > Themed(8)) {
            TruncateString(&name, B_TRUNCATE_END, room);
            SetHighColor(c.muted ? Rgb(190, 194, 200) : Rgb(245, 246, 248));
            DrawString(name.String(), BPoint(strip.left + Themed(12),
                                             strip.top + Themed(11)));
        }
        // Muted badge on the strip's right corner.
        if (c.muted && strip.Width() > Themed(30)) {
            SetHighColor(Rgb(232, 120, 120));
            DrawString("M", BPoint(strip.right - Themed(11),
                                   strip.top + Themed(11)));
        }
        // Stretch badge (M7.2): the clip does not play 1:1 — the ratio the
        // stretch drag set, so the reason the waveform no longer lines up with
        // the source is on the clip rather than in the user's memory. Right-
        // aligned like the mute badge, and left of it when both show.
        if (ClipIsStretched(c) && strip.Width() > Themed(44)) {
            char label[16];
            std::snprintf(label, sizeof(label), "\xC3\x97%.2f",
                          ClipStretchRatio(c));        // UTF-8 multiplication sign
            const float w = StringWidth(label);
            const float x = strip.right - Themed(4) - w
                            - (c.muted ? Themed(12) : 0.0f);
            if (x > strip.left + Themed(2)) {
                SetHighColor(Rgb(255, 226, 150));
                DrawString(label, BPoint(x, strip.top + Themed(11)));
            }
        }
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

    // Pointer feedback (M2.1), drawn last so it sits over the border: the
    // grabbed edge lights up, and the scissors show where the cut would land.
    // Not while a drag owns the screen (the hover state is stale then).
    if (fHoverValid && c.id == fHover.clip) {
        if (fTool == Tool::Pointer) DrawHoverEdge(block, fHover.zone);
        DrawScissorsHover(block);
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
        // A muted region (M2.5) draws desaturated, like a muted clip.
        const float fillShade = mc.muted ? 0.30f : 0.55f;
        SetHighColor(Rgb((uint8)(base.red * fillShade),
                         (uint8)(base.green * fillShade),
                         (uint8)(base.blue * fillShade)));
        FillRoundRect(block, Themed(5), Themed(5));
        // Name strip across the top.
        if (block.Height() > Themed(17) && block.Width() > Themed(10)) {
            BRect strip(block.left + Themed(1), block.top + Themed(1), block.right - Themed(1),
                        block.top + Themed(15));
            const float stripShade = mc.muted ? 0.22f : 0.32f;
            SetHighColor(Rgb((uint8)(base.red * stripShade),
                             (uint8)(base.green * stripShade),
                             (uint8)(base.blue * stripShade)));
            FillRect(strip);
            BString name(t.name.c_str());
            const float room = strip.Width() - Themed(6)
                             - (mc.muted ? Themed(14) : 0.0f);
            if (room > Themed(8)) {
                TruncateString(&name, B_TRUNCATE_END, room);
                SetHighColor(mc.muted ? Rgb(186, 190, 196) : Rgb(240, 242, 245));
                DrawString(name.String(), BPoint(strip.left + Themed(4),
                                                 strip.top + Themed(11)));
            }
            if (mc.muted && strip.Width() > Themed(30)) {   // mute badge
                SetHighColor(Rgb(232, 120, 120));
                DrawString("M", BPoint(strip.right - Themed(11),
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
        // Pointer feedback, exactly as an audio clip gets it (M2.1).
        if (fHoverValid && mc.id == fHover.clip) {
            if (fTool == Tool::Pointer) DrawHoverEdge(block, fHover.zone);
            DrawScissorsHover(block);
        }
    }
}

// Paint the min/max envelope inside a clip block: one vertical line per pixel
// column, from the column's min sample to its max. Reads a handful of peak
// buckets per column (never scans the audio). No cache for this clip's source
// -> just the flat filled block.
void TimelineView::DrawClipWave(const Clip& c, BRect block, BRect update,
                                const ClipFades& ef) {
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
    // regardless of the file's sample rate — and by the clip's stretch, which
    // walks the source that much faster or slower (M7.2), so a stretched clip
    // draws the material it actually plays.
    const double projRate = fProject->sampleRate;
    const double srcRate  = pc.SampleRate();
    const double toSrc = ((srcRate > 0 && projRate > 0) ? srcRate / projRate : 1.0)
                         / ClipStretchRatio(c);

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
        // The envelope this column sounds at (M2.5): clip gain and the
        // effective fades with their shapes — the SAME helper the engine and
        // the exporter apply, so the drawn waveform is the level that plays
        // rather than the raw source. A muted clip draws its envelope too
        // (editing it is still possible), just in the muted wave colour.
        const float env = c.gain * ClipFadeEnvelope(
            tf0 - c.startFrame, c.lengthFrames, ef.fadeIn, ef.inShape,
            ef.fadeOut, ef.outShape);
        const float yMax = mid - pk.max * half * env;   // max amplitude -> up
        const float yMin = mid - pk.min * half * env;   // min amplitude -> down
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
