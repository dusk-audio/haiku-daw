// TimelineView — the custom-drawn arrangement view.
//
// Draws (left→right = time, top→bottom = tracks): a time ruler, one lane per
// track, and each clip as a block. Waveforms (from PeakCache) land in M4c;
// playhead + click-seek in M4d; clip drag in M4e. For now it renders a static
// snapshot of the Project it is given (non-owning pointer).
//
// Coordinate model: a frame maps to x via FrameToX(); horizontal scroll is a
// frame offset, zoom is frames-per-pixel. The header column (HeaderWidth()) is
// a fixed gutter on the left; time content starts after it.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"
#include "../model/Commands.h"
#include "../model/Grid.h"
#include "../model/SnapGrid.h"     // the arrange grid the menu offers
#include "../model/Crossfade.h"   // ClipFades (the per-track fade cache)
#include "../model/KeyMap.h"       // the one keyboard map (M3.2)
#include "UiMetrics.h"             // HeaderWidth() (the peak invalidation)

#include <Cursor.h>
#include <ScrollBar.h>
#include <View.h>

#include <array>
#include <functional>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace daw {

class Recorder;   // engine capture source (live waveform envelope)

// A BScrollBar that reports its value instead of scrolling a target view: the
// timeline scrolls by frame offset, not by moving its bounds, so the default
// BScrollBar behaviour (ScrollBy on a target) is the wrong mechanism. With no
// target set, BScrollBar only updates itself -- and calls this virtual, which
// is the hook.
class TimelineScrollBar : public BScrollBar {
public:
    TimelineScrollBar(const char* name, orientation dir, float min, float max,
                      std::function<void(float)> onValue);
    void ValueChanged(float newValue) override;

private:
    std::function<void(float)> fOnValue;
};

// Clip region-operation + track-freeze requests posted to the main window,
// which owns the engine/decode and issues the resulting command(s). Each region
// message carries int64 "track" + int64 "clip"; freeze carries int64 "track"
// + bool "freeze".
constexpr uint32 kMsgRegionNormalize = 'rnrm';
constexpr uint32 kMsgRegionReverse   = 'rrev';
constexpr uint32 kMsgRegionStrip     = 'rstp';
constexpr uint32 kMsgFreezeTrack     = 'frtk';

// A .mid file dropped on the timeline from Tracker (or any B_SIMPLE_DATA
// source): string "path" + int64 "start" (the snapped drop frame). Audio drops
// reuse the sample browser's kMsgBrowserImport instead.
constexpr uint32 kMsgDropMidi = 'dpmd';

class TimelineView : public BView, public ThemeAware {
public:
    void ApplyTheme() override;   // T1: the cached lane colour

    // BView already has a Frame() method; without this typedef every
    // unqualified `Frame` in this class would bind to BView::Frame() instead
    // of the model's frame type. A member typedef hides the inherited name.
    using Frame = daw::Frame;

    // Mutable project + command stack: header controls edit the model through
    // the stack (never in place), so every change is undoable.
    TimelineView(BRect frame, Project* project, CommandStack* stack);

    void Draw(BRect updateRect) override;
    void MessageReceived(BMessage* msg) override;   // mouse-wheel vertical scroll
    void MouseDown(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseUp(BPoint where) override;
    void KeyDown(const char* bytes, int32 numBytes) override;
    void AttachedToWindow() override;
    // A plain BView's minimum size IS its current size, and this view is an
    // item in the pane split: left unsaid, every time it grew the split's (and
    // so the window layout's) minimum grew with it, until the transport bar
    // was laid out wider than the window and the master meter -- pinned to the
    // bar's right edge -- sat outside the visible window. M3.3 found it; these
    // two say what the view actually needs (UI_GUIDELINES §4).
    BSize MinSize() override { return BSize(Themed(200.0f), Themed(120.0f)); }
    BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED); }

    // Editing tools (M2.3): the palette strip across the top of the view picks
    // the click behaviour, exactly as the piano roll's does.
    enum class Tool { Pointer, Pencil, Scissors, Glue, Mute, Fade };
    Tool        ActiveTool() const { return fTool; }
    void        SetTool(Tool t);
    static const char* ToolName(Tool t);

    // What a press at `where` would act on -- one answer, shared by MouseDown,
    // the hover highlight and the cursor, so the three cannot disagree. The
    // zone depends on the modifiers (Ctrl = gain, Alt = slip), so they are a
    // parameter: the one-argument form reads the event's, and CursorFor passes
    // the state it was handed (a caller that asks for the Alt variant must get
    // the Alt answer, not whatever the keyboard happens to be doing).
    enum class Zone { None, Body, TrimLeft, TrimRight, FadeIn, FadeOut, Gain, Slip };
    struct Hit {
        int     lane  = -1;                    // track index, -1 = none
        TrackId track = kInvalidTrackId;
        ClipId  clip  = kInvalidClipId;        // audio clip or MIDI region
        bool    midi  = false;
        Zone    zone  = Zone::None;
    };
    Hit HitTest(BPoint where) const;
    Hit HitTest(BPoint where, uint32 mods) const;
    std::size_t SelectionCount() const { return fSelClips.size(); }

    // The cursor this pointer state wants. `mods` is passed rather than read so
    // a test can ask for the Alt/Ctrl variants without a keyboard.
    enum class Pointer { Default, Move, Trim, Gain, Slip, Split, Fade, Pencil,
                         Glue, Mute, LaneSize };
    Pointer CursorFor(BPoint where, uint32 mods) const;
    Pointer ShownCursor() const { return fCursor; }

    // The snap grid (M2.3). Public so the functional test can drive what the
    // popup menu sets, and so the indicator's label has one definition.
    void     SetSnapGrid(SnapGrid g);
    SnapGrid Snap() const { return fSnap; }
    const char* SnapLabel() const { return SnapGridLabel(fSnap); }

    // Split every clip/region the playhead falls inside (one undo step): the
    // selection's when there is one, else every track's. `S`.
    void SplitAtPlayhead();

    // --- time-range selection (M2.4) ---------------------------------------
    //
    // The range is VIEW state (nothing about a selection belongs in the
    // project file) and project-wide: no per-track selection exists until
    // M2.6, so a range edit applies to every track. Public so the Edit menu
    // and the functional tests drive exactly what the gestures set.
    void  SetTimeRange(Frame a, Frame b);   // ordered; an empty span clears it
    void  ClearTimeRange();
    bool  HasTimeRange() const { return fRangeValid; }
    Frame RangeStart() const { return fRangeA; }
    Frame RangeEnd()   const { return fRangeB; }

    // The Edit menu's operations. Copy fills the clipboard from the range (or
    // from the clip selection when there is no range); Cut is Copy + Delete;
    // Delete clears the range (or the selection); Insert Silence ripples the
    // range; Paste drops the clipboard at the playhead; Duplicate re-states
    // the selection (or the range) one span later; Select All takes every clip
    // and region. Each is one undo step.
    void  RangeCopy();
    void  RangeCut();
    void  RangeDelete();
    void  RangeInsertSilence();
    void  SelectAllClips();
    void  DuplicateSelection();

    // The clip-depth actions (M2.5) the context menu runs, as methods: a popup
    // menu cannot be driven without a real pointer, so the functional test
    // calls the same code the menu entry does rather than a hand-made model
    // edit. (Same pattern as SetTool/SetSnapGrid in the previous package.)
    void  ToggleClipMute(TrackId track, ClipId clip, bool midi);
    void  NextClipColour(TrackId track, ClipId clip);
    void  SetClipFadeShapeAt(TrackId track, ClipId clip, FadeShape shape);

    // Keyboard nudge (`,` `.`): move the selection by one grid step, `dir`
    // -1/+1. No-op without a selection.
    void  NudgeSelection(int dir);

    int   ClipboardCount() const { return (int)fClipboard.size(); }  // tests

    // --- track depth (M2.6) ------------------------------------------------
    //
    // The header's controls, named once. The drawing, the click path, the
    // hover, the cursor and the functional test all read HeaderControlAt, so
    // a control cannot be drawn somewhere and clicked somewhere else.
    enum class HeaderControl { None, Mute, Solo, Arm, Monitor, Pan, Gain,
                               Colour, Input, Fold };
    HeaderControl HeaderControlAt(int lane, BPoint where) const;
    // Its rect, for the pickers' anchors and for a test that has to press ON a
    // control without opening the (synchronous) popup it owns.
    BRect HeaderRectFor(int lane, HeaderControl which) const;
    // A lane's rectangle (the geometry the lane-edge grab measures against).
    BRect LaneFrame(int index) const { return LaneRect(index); }
    // The lane whose BOTTOM edge is under `where` in the header gutter (the
    // resize grab), or -1. Public: the cursor, the hover and the test all use
    // the same answer.
    int   LaneEdgeAt(BPoint where) const;

    // The track-depth actions, shared by the header, the menus and the tests
    // (a popup menu cannot be driven by the suite -- the established pattern).
    void  SetTrackColour(TrackId track, int colorIndex);
    void  SetTrackInput(TrackId track, InputSource input);
    void  SetTrackCollapsed(TrackId track, bool collapsed);
    void  ToggleTrackCollapsed(TrackId track);
    void  ExclusiveSolo(TrackId track);      // Alt-click on the header's S
    void  DuplicateTrack(TrackId track);     // the copy is selected + shown
    void  DuplicateSelectedTrack();          // the Track menu's entry

    // In-place rename (M2.6). The name field in the header becomes a text
    // field; Enter commits ONE SetTrackNameCommand, Esc cancels, a press
    // outside commits first. Public so the test can read the edit state.
    bool        RenamingInPlace() const { return fRenameTrack != kInvalidTrackId; }
    TrackId     RenamingTrack()   const { return fRenameTrack; }
    const char* RenameBuffer()    const { return fRenameText.c_str(); }
    void        BeginTrackRename(TrackId track);

    // --- markers (M2.7) -----------------------------------------------------
    //
    // The marker lane: a flag strip between the ruler and the lanes, toggled
    // from the View menu (MSG_MARKER_LANE) and Shift-M. Its height is part of
    // ContentTop() while it is on.
    void SetMarkerLane(bool on);
    bool MarkerLaneOn() const { return fMarkerLane; }

    // The marker actions, one definition each: the `m` key, the ruler's
    // context menu, the marker lane and the markers window all come through
    // here, and every one of them posts kMsgUiRefresh so an open markers
    // window is never stale.
    void AddMarkerAt(Frame frame);                          // "Marker N"
    void RemoveMarkerAt(Frame frame, const std::string& name);
    void SeekTo(Frame frame);        // move the playhead AND the drawn line
    // The transport bar's RTZ and Loop buttons run exactly what the keyboard
    // map's Home/Enter and L run (M3.1) — one implementation, two ways in.
    void ReturnToZero();             // playhead to the start, view with it
    void ToggleLoop();               // loop on/off (adopts a range if none)

    // Horizontal zoom (multiply frames-per-pixel, clamped) and pan.
    void ZoomBy(double factor);
    // Zoom about the frame under `x`, so the point under the pointer stays put.
    void ZoomAnchoredAt(double factor, float x);
    void PanBy(Frame deltaFrames);
    void ZoomToFit();               // fit the whole project in the view width
    void ScrollVerticalBy(float dy);// vertical track scroll (clamped)
    void ScrollToFrame(Frame f);    // absolute scroll (the scrollbar's setter)
    void ScrollToY(float y);

    // Frame <-> pixel mapping (content area, i.e. right of the header gutter).
    float FrameToX(Frame f) const;
    Frame XToFrame(float x) const;

    // Top of the lane area: the tool strip, then the ruler, then the marker
    // lane when it is on.
    float ContentTop() const {
        return RulerBottom() + (fMarkerLane ? MarkerLaneHeight() : 0.0f);
    }
    // Where the ruler ends: the marker lane (when shown) sits between this and
    // ContentTop(), so the ruler's own hit tests must use THIS, not ContentTop.
    float RulerBottom() const { return TimelineContentTop(); }
    // Where the project's content ends (last clip/region, playhead, loop,
    // punch, markers): the horizontal scroll may go this far, so the whole
    // arrangement can be scrolled past the left edge.
    Frame ContentEndFrame() const;
    // Visible-frame introspection for the functional tests.
    double FramesPerPixel() const { return fFramesPerPixel; }
    Frame  ScrollFrame()     const { return fScrollFrame; }
    float  ScrollY()         const { return fScrollY; }
    float  VisibleFrames()   const;

    // Musical grid built from the project; Snapped() snaps a frame to it
    // unless Shift is held (free placement).
    Grid  GridOf() const;
    Frame Snapped(Frame f) const;

    // Paste the clipboard (many clips) at the playhead — onto the selected
    // track when it matches the item's kind, else the first track that does
    // (Edit > Paste / Cmd-V, and the right-click "Paste here" flow).
    void  PasteAtPlayhead();

    void SetProject(Project* p) {
        fProject = p;
        fFadeCache.clear();   // a different project, different clips
        // Selections are ids into the PROJECT that made them: clip ids from the
        // previous project would silently "select" whatever the new one has
        // under those ids, and a range would point at frames that no longer
        // mean anything (UI_GUIDELINES: stale content is a bug).
        fSelClips.clear();
        fRangeValid = false;
        fRangeA = fRangeB = 0;
        // An in-place rename addresses a track by id in the project that made
        // it; a new project's ids mean something else (stale content is a bug).
        CancelTrackRename();
        Invalidate();
    }

    // Waveform envelopes, keyed by clip source path. Non-owning; built once
    // on import (M4c) and shared across clips that reference the same file.
    using PeakMap = std::map<std::string, PeakCache>;
    void SetPeaks(const PeakMap* peaks) { fPeaks = peaks; Invalidate(); }

    // Move the playhead. Invalidates only the old + new columns, so the 60 Hz
    // poll doesn't repaint the whole view each tick.
    void SetPlayhead(Frame f);
    // The DRAWN playhead (what the line is at), not the model's -- the two can
    // differ if a seek forgets to move the line, which is what this exposes.
    Frame Playhead() const { return fPlayhead; }
    void SetFollow(bool on) { fFollow = on; }   // chase the playhead in view
    void SetMonitorInput(bool on) { fMonitorInput = on; Invalidate(); }  // lane "I" lamp
    void CycleAuto(TrackId id);                 // inspector Auto button
    // The track's automation-lane mode (0 Off / 1 Gain / 2 Pan / 3+ an fx lane).
    // Read by the mixer's strip payload, so its Auto button's lamp is lit from
    // the mode this view actually holds rather than from a second copy of it.
    int  AutoModeFor(TrackId id) const {
        auto it = fAutoMode.find(id);
        return it == fAutoMode.end() ? 0 : it->second;
    }

    // Currently selected track (drives the inspector; -1 if none).
    TrackId SelectedTrack() const { return fSelectedTrack; }

    // Per-track output peaks (from the engine) for the header meters. Cleared
    // when playback stops. Keyed by TrackId -> (peakL, peakR) in [0, 1+].
    // The peaks are drawn in the header column only (the per-track meters), so
    // invalidate THAT -- at 60 Hz while playing, a full-timeline invalidate was
    // repainting every lane for a meter tick.
    void SetTrackPeaks(const std::map<TrackId, std::pair<float, float>>& peaks) {
        fTrackPeaks = peaks;
        Invalidate(BRect(0, 0, HeaderWidth(), Bounds().bottom));
    }
    void ClearTrackPeaks() {
        fTrackPeaks.clear();
        Invalidate(BRect(0, 0, HeaderWidth(), Bounds().bottom));
    }
    // Clicking a lane's meter clears its clip lamp (the master meter's own
    // gesture); the lamp itself latches at digital full scale, per channel.
    void ClearClipLamps(TrackId id);

    // Live recording region drawn as a growing block on every armed track
    // while a take is being captured (before the real clips exist). Pass
    // active=false to clear it.
    void SetRecording(bool active, Frame start, Frame length);

    // Live take content drawn inside the recording region while capturing:
    // MIDI notes (clip-relative to the record start) for the armed MIDI tracks,
    // and the audio waveform envelope streamed from the Recorder. Cleared by
    // SetRecording(false, ...).
    // Live take content per armed MIDI track. Keyed by track because inputs are
    // demuxed: with two keyboards each track is capturing its own notes, so a
    // single shared list would draw the wrong take on both.
    void SetLiveMidiNotes(std::map<TrackId, std::vector<MidiNote>> perTrack) {
        fLiveNotes = std::move(perTrack);
    }
    void SetLiveAudio(const Recorder* rec, double projectRate,
                      std::map<TrackId, int> takeFor) {
        fLiveRec      = rec;
        fLiveProjRate = projectRate;
        fLiveTake     = std::move(takeFor);
    }

private:
    void DrawRuler(BRect update);
    void DrawToolbar();                 // the tool palette strip
    void DrawLanes(BRect update);
    // Iterate visible bar/beat gridlines: fn(x, isBar, barNumber).
    void ForEachGridLine(const std::function<void(float, bool, long)>& fn) const;
    void DrawTrackHeader(const Track& t, BRect lane);
    void DrawTrackHeaderCompact(const Track& t, BRect lane);  // collapsed lane
    void DrawNameField(const Track& t, BRect lane);   // the in-place rename box
    void DrawLiveMidi(BRect region, TrackId track);   // in-progress notes while recording
    void DrawLiveAudio(BRect region, TrackId track);  // in-progress waveform while recording
    void DrawMidiNotes(const Track& t, BRect lane);
    // `ef` holds the EFFECTIVE fades (the clip's own combined with any
    // auto-crossfade from overlapping a neighbour) AND the shape each one
    // renders with, so the drawing — ramps and waveform envelope — matches
    // what the engine and exporter render. See model/Crossfade.h.
    void DrawClip(const Clip& c, BRect lane, rgb_color base,
                  const ClipFades& ef, BRect update);
    void DrawClipWave(const Clip& c, BRect block, BRect update,
                      const ClipFades& ef);

    // --- the tool strip (M2.3) --------------------------------------------
    BRect ToolRect(int i) const;        // palette button i
    int   ToolButtonAt(BPoint where) const;   // -1 if not on one
    BRect GridRect() const;             // the snap field
    BRect ZoomOutRect() const;
    BRect ZoomInRect() const;
    void  HandleToolbarClick(BPoint where);
    void  OpenGridMenu();               // the snap popup
    void  DrawToolButton(int i);        // button + glyph + hover

    // --- the header's pickers (M2.6) ---------------------------------------
    void  OpenColourPicker(TrackId track, BPoint where);
    void  OpenInputPicker(TrackId track, BPoint where);
    // In-place rename internals: the key handler is the field's own, and only
    // the commit and the cancel end the edit.
    bool  HandleRenameKey(const char* bytes, int32 numBytes);
    void  CommitTrackRename();
    void  CancelTrackRename();
    TrackId     fRenameTrack = kInvalidTrackId;
    std::string fRenameText;
    int         fRenameCaret = 0;       // byte offset into fRenameText

    // --- the marker lane (M2.7) --------------------------------------------
    void   DrawMarkerLane();
    void   HandleMarkerLaneClick(BPoint where, bool rightClick);
    // The marker whose flag or name is under a point on the marker lane.
    const Marker* MarkerInLane(BPoint where) const;
    bool   fMarkerLane = false;

    // --- the tools' gestures ----------------------------------------------
    void ScissorsAt(const Hit& hit, BPoint where);
    void GlueAt(const Hit& hit);
    void MuteAt(const Hit& hit);
    void FadeStart(const Hit& hit, BPoint where);      // drag the nearer fade
    bool PencilAt(const Hit& hit, BPoint where);       // true = took the click
    void CommitPencil(BPoint where);                   // mouse-up of a draw
    // The region a pencil drag would create: press frame to release frame.
    Frame fPencilStart = 0;
    Frame fPencilLen   = 0;

    // --- hover, cursor and tooltips (M2.1) --------------------------------
    // Recomputed on every mouse move; the drawing reads the same Hit the click
    // path uses, and only a CHANGE repaints (a pointer move over a big project
    // must not repaint the world).
    void UpdateHover(BPoint where, uint32 mods, uint32 transit);
    void ApplyCursor(Pointer p);
    void SetTip(const char* text);      // tooltip, only when it changes
    Hit     fHover;                     // what is under the cursor
    BPoint  fHoverPos{ -1.0f, -1.0f };
    bool    fHoverValid  = false;
    int     fHoverToolBtn = -1;         // palette button under the cursor
    HeaderControl fHoverHeader = HeaderControl::None;   // header control under it
    bool    fHoverLaneEdge = false;     // the lane resize edge is under it
    float   fSplitHoverX  = -1.0f;      // scissors' future cut line
    std::string fTip;
    Pointer fCursor = Pointer::Default;
    std::map<int, std::unique_ptr<BCursor>> fCursors;
    const BCursor* CursorObject(Pointer p);   // built once, on first use
    void DrawHoverEdge(const BRect& block, Zone zone);   // clip-edge highlight
    void DrawScissorsHover(const BRect& block);          // the future cut line

    // --- navigation (M2.2) -------------------------------------------------
    void SyncScrollBars();              // ranges/proportions/values
    void LayoutScrollBars();            // frames (a layout view does not
                                        // reposition children by follow modes)
    void FrameResized(float newWidth, float newHeight) override;
    TimelineScrollBar* fHBar = nullptr;
    TimelineScrollBar* fVBar = nullptr;
    float fBarThickness = 14.0f;        // the stock scrollbar's, from GetPreferredSize
    bool  fSyncingBars  = false;        // our own SetValue must not echo back
    float fBarLastX = -1.0f, fBarLastY = -1.0f, fBarLastRangeX = -1.0f;
    float fBarLastRangeY = -1.0f, fBarLastPropX = -1.0f, fBarLastPropY = -1.0f;

    // --- editing tools / snap (M2.3) ---------------------------------------
    Tool     fTool = Tool::Pointer;
    SnapGrid fSnap;                     // 1/16, as the editor always snapped

    // Effective fades per track, cached: ComputeCrossfades allocates, and it
    // changes only when the clips do. The cache holds the tuples it was built
    // from, so the common case is an exact O(n) comparison with no allocation
    // instead of one allocation per lane per draw.
    struct FadeCache {
        std::vector<std::array<long long, 5>> key;
        std::vector<ClipFades>                fades;
    };
    std::map<TrackId, FadeCache> fFadeCache;
    const std::vector<ClipFades>& FadesFor(const Track& t);
    // Overlap X + tint, drawn after a lane's clips so the earlier clip's ramp is
    // not buried under the later clip's block.
    void DrawCrossfades(const Track& t, BRect lane);
    void DrawPlayhead();
    void DrawDragGhost();   // clip-move preview rectangle

    // Lane geometry + header hit-testing.
    BRect LaneRect(int index) const;
    int   TrackIndexAt(BPoint where) const;             // -1 if none
    void  HandleHeaderClick(const Track& t, BRect lane, BPoint where);
    void  HandleRulerMenu(BPoint where);        // tempo/meter/marker menu
    const Marker* MarkerAt(BPoint where) const; // ruler marker under the cursor
    void  JumpToMarker(int dir);                // -1 = prev, +1 = next, from playhead
    void  LoopBetweenMarkers();                 // cycle the markers bracketing the playhead
    Frame BarStartFrameAt(Frame f) const;       // nearest bar boundary frame

    // Live fader/pan drag. During a drag we preview by writing the model
    // directly; on release we restore the original and push ONE command, so
    // the whole gesture is a single clean undo step.
    enum class Drag { None, Gain, Pan, Clip, ClipResize, ClipResizeLeft,
                      ClipFadeIn, ClipFadeOut,
                      ClipGain, Note, NoteResize, NoteVelocity, RulerLoop,
                      RulerPunch, Pencil, Slip, RangeSelect, LaneResize,
                      ClipStretch };
    void  PreviewDrag(BPoint where);   // apply the dragged value for feedback
    int   PitchAt(BRect lane, float y) const;   // y -> MIDI pitch
    Drag    fDrag      = Drag::None;
    TrackId fDragTrack = kInvalidTrackId;
    int     fDragLane  = -1;
    float   fDragOrig  = 0.0f;         // gain/pan value at drag start, for undo
    float   fDragGrabY = 0.0f;         // cursor y at drag start (pan knob = vertical)

    // Clip drag state.
    ClipId  fDragClip        = kInvalidClipId;
    bool    fDragIsMidiClip  = false;  // dragging a MIDI region (vs audio clip)
    Frame   fDragClipOrig    = 0;      // clip startFrame at drag start
    // An audio clip's read offset at drag start. Trimming the FRONT has to
    // advance it by the same delta, or the audio slides against the timeline
    // instead of being trimmed.
    Frame   fDragClipSrcOrig = 0;
    Frame   fDragClipOrigLen = 0;      // clip lengthFrames at drag start
    // Stretch drag (Cmd/Alt on the right edge, M7.2): the ratio at drag start.
    // The dragged ratio is derived from the PRE-drag length + ratio (the span
    // the clip references today), never from the values the preview has already
    // written into the model — a drag that read its own preview would compound.
    double  fDragRatioOrig   = 1.0;
    Frame   fDragGrabOffset  = 0;      // grabbed-frame - clip.startFrame
    Frame   fDragFadeInOrig  = 0;      // clip fades at drag start (for undo)
    Frame   fDragFadeOutOrig = 0;
    // Slip (Alt-drag): the read offset at drag start and the pointer x there,
    // so the whole gesture is one offset delta committed on release.
    Frame   fDragSlipOrig    = 0;
    float   fDragSlipGrabX   = 0.0f;
    // Clip-move ghost (follows the cursor across lanes; model isn't touched
    // until drop).
    int     fDragCurLane  = -1;        // target lane under the cursor
    Frame   fDragCurStart = 0;         // previewed start frame

    // Note drag state.
    int      fDragNote        = -1;    // index into the track's notes
    int      fDragPitchOffset = 0;     // note pitch - grab-point pitch
    MidiNote fDragNoteOrig;            // note at drag start, for undo

    // Ruler loop-drag state.
    Frame    fLoopAnchor  = 0;         // frame where a loop drag started
    bool     fLoopDragged = false;     // did the pointer move (drag vs click)?

    // Clipboard (right-click Copy -> Edit > Paste, and the range operations).
    // Many clips (M2.4): a range copy arrives with the pieces' starts relative
    // to the earliest, so the internal spacing survives the paste.
    struct ClipItem {
        bool     midi = false;
        TrackId  track = kInvalidTrackId;   // where it came from (a range copy)
        Clip     audio;
        MidiClip region;
    };
    void  SetClipboardToClip(const Clip& c);          // one audio clip
    void  SetClipboardToRegion(const MidiClip& c);    // one MIDI region
    void  SetClipboardToSelection();                  // the selected clips
    // `toSourceTracks` places each item on the track it came from (a range
    // duplicate); otherwise the paste target rule below applies.
    void  PasteItems(const std::vector<ClipItem>& items, Frame at,
                     bool toSourceTracks = false);
    TrackId PasteTargetFor(bool midi) const;          // selected track, else first
    int      ContextMenu(BPoint where, bool withSplit = false,
                         bool withTake = false) const;   // 0=Copy 1=Delete 2=Split 3=NextTake
    // Richer audio-clip menu (adds region ops + the clip-depth entries);
    // returns the chosen item's label ("" if dismissed) so callers switch on
    // meaning, not a fragile index.
    std::string AudioClipMenu(BPoint where, bool withTake) const;
    bool     PastePopup(BPoint where) const;    // "Paste here" -> true if chosen
    void     PasteToTrack(TrackId track, Frame at, TrackType type);
    // The keyboard map's actions this view runs itself (M3.2). The ones that
    // change state the main window owns (record, click) are posted there.
    // (ReturnToZero and ToggleLoop are public above: the transport bar's
    // buttons run the same code.)
    void  DropMarker();                // M
    std::vector<ClipItem> fClipboard;

    // The range selection (M2.4). Empty (fRangeValid false) until a drag on
    // empty lane content or an Alt-drag on the ruler sets it.
    Frame fRangeA = 0, fRangeB = 0;
    bool  fRangeValid = false;
    Frame fRangeAnchor = 0;        // where a range drag started
    bool  fRangeDragged = false;   // did that drag pass the threshold?
    void  BeginRangeDrag(BPoint where);   // press on empty lane content
    void  SetSelectedTrack(TrackId id);   // click in a lane selects its track
    // Draw the range over the lane area (Draw()) / on the ruler (DrawRuler()).
    void  DrawTimeRange();
    std::string ClipNameOf(const Clip& c) const;   // Clip.name or the file's

    // Open the piano roll for one MIDI region (notes edited clip-relative).
    void OpenPianoRollForClip(TrackId track, ClipId clip);

    // Automation editing. fAutoMode[track] = 0 off / 1 gain / 2 pan /
    // 3+ = fx-parameter lane (fxAuto[mode-3]); cycled by the header "Auto" box.
    // A resolved lane reference: the lane + its value range + static default.
    struct AutoRef { const AutomationLane* lane = nullptr;
                     float mn = 0, mx = 1, def = 0; int fxIndex = -1; };
    bool  AutoRefFor(const Track& t, int mode, AutoRef* out) const;
    void  DrawAutomation(const Track& t, BRect lane, int mode);
    void  HandleAutoMouseDown(const Track& t, BRect lane, int idx, BPoint where,
                              bool rightClick);
    void  CommitAuto(TrackId track, int mode, int fxIndex,
                     const AutomationLane& lane);
    int   AutoPointAt(const AutomationLane& al, BRect lane, float mn, float mx,
                      BPoint where) const;
    float AutoValueToY(BRect lane, float mn, float mx, float v) const;
    float AutoYToValue(BRect lane, float mn, float mx, float y) const;
    std::map<TrackId, int> fAutoMode;
    bool           fAutoDragging = false;
    TrackId        fAutoTrack = kInvalidTrackId;
    int            fAutoFx = -1;            // >=0 = fxAuto index (else gain/pan)
    float          fAutoMn = 0, fAutoMx = 1;
    Frame          fAutoDragFrame = 0;      // frame-key of the dragged breakpoint
    AutomationLane fAutoOrig;               // lane at drag start (for undo)

    // Multi-select over audio clips. Clip ids are project-unique, so a plain
    // set suffices; the track is found by scan when needed.
    std::map<TrackId, std::pair<float, float>> fTrackPeaks;   // header meters
    // Per-track meter state (package 52): a peak hold and a clip latch per
    // channel, so the lane meter reads like the mixer strips' -- a tick that
    // falls, and a lamp that latches until it is clicked. Keyed by track id, so
    // a refresh cannot hand one track's hold to another.
    struct TrackMeterState {
        PeakHold  holdL, holdR;
        ClipLatch clipL, clipR;
    };
    std::map<TrackId, TrackMeterState> fTrackMeter;

    std::set<ClipId> fSelClips;
    // Captured start of each selected MIDI region at drag-start, for group move.
    std::map<ClipId, Frame> fMidiMoveOrig;
    bool  ClipSelected(ClipId id) const { return fSelClips.count(id) != 0; }
    void  DeleteSelection();       // MacroCommand remove of all selected clips
    Track* TrackOfClip(ClipId id) const;   // owning track, or nullptr

    // Rubber-band box select (drag on empty lane content).
    bool   fBanding = false;
    BPoint fBandA, fBandB;

    // Multi-clip move: all selected clips shift by one frame delta (ghost
    // preview; model untouched until drop).
    bool   fMultiMove  = false;
    Frame  fMultiDelta = 0;

    Project*       fProject;          // non-owning, mutable via fStack
    CommandStack*  fStack;            // non-owning
    const PeakMap* fPeaks = nullptr;  // non-owning
    double         fFramesPerPixel;   // horizontal zoom
    Frame          fScrollFrame;      // leftmost visible frame (content x=0)
    float          fScrollY = 0.0f;   // vertical track-scroll offset (pixels)
    float          ContentHeight() const;   // total stacked lane height
    Frame          fPlayhead = 0;     // in project frames

    // Live recording region (no clips yet); drawn on every armed track.
    bool           fRecording = false;
    bool           fFollow    = true;   // auto-scroll to keep the playhead in view
    TrackId        fSelectedTrack = kInvalidTrackId;  // inspector focus + highlight
    bool           fMonitorInput  = false;            // global input-monitor lamp
    Frame          fRecStart  = 0;
    Frame          fRecLen    = 0;
    // Live take content while recording (see SetLiveMidiNotes / SetLiveAudio).
    // Notes are clip-relative to fRecStart, per capturing track.
    std::map<TrackId, std::vector<MidiNote>> fLiveNotes;
    const Recorder*       fLiveRec = nullptr; // non-owning; valid during capture
    double                fLiveProjRate = 48000.0;
    // Which TAKE of the capture each armed audio track is being recorded into
    // (M5): one capture writes one file per track, so the waveform drawn on a
    // lane has to be that track's take, not the capture's first one.
    std::map<TrackId, int> fLiveTake;
};

} // namespace daw
