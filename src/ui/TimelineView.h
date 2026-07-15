// TimelineView — the custom-drawn arrangement view.
//
// Draws (left→right = time, top→bottom = tracks): a time ruler, one lane per
// track, and each clip as a block. Waveforms (from PeakCache) land in M4c;
// playhead + click-seek in M4d; clip drag in M4e. For now it renders a static
// snapshot of the Project it is given (non-owning pointer).
//
// Coordinate model: a frame maps to x via FrameToX(); horizontal scroll is a
// frame offset, zoom is frames-per-pixel. The header column (kHeaderWidth) is
// a fixed gutter on the left; time content starts after it.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"
#include "../model/Commands.h"
#include "../model/Grid.h"

#include <View.h>

#include <functional>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace daw {

// Clip region-operation + track-freeze requests posted to the main window,
// which owns the engine/decode and issues the resulting command(s). Each region
// message carries int64 "track" + int64 "clip"; freeze carries int64 "track"
// + bool "freeze".
constexpr uint32 kMsgRegionNormalize = 'rnrm';
constexpr uint32 kMsgRegionReverse   = 'rrev';
constexpr uint32 kMsgRegionStrip     = 'rstp';
constexpr uint32 kMsgFreezeTrack     = 'frtk';

class TimelineView : public BView {
public:
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

    // Horizontal zoom (multiply frames-per-pixel, clamped) and pan.
    void ZoomBy(double factor);
    void PanBy(Frame deltaFrames);
    void ZoomToFit();               // fit the whole project in the view width
    void ScrollVerticalBy(float dy);// vertical track scroll (clamped)

    // Frame <-> pixel mapping (content area, i.e. right of the header gutter).
    float FrameToX(Frame f) const;
    Frame XToFrame(float x) const;

    // Musical grid built from the project; Snapped() snaps a frame to it
    // unless Shift is held (free placement).
    Grid  GridOf() const;
    Frame Snapped(Frame f) const;

    // Paste the clipboard clip/note at the playhead (Edit > Paste).
    void  PasteAtPlayhead();

    void SetProject(Project* p) { fProject = p; Invalidate(); }

    // Waveform envelopes, keyed by clip source path. Non-owning; built once
    // on import (M4c) and shared across clips that reference the same file.
    using PeakMap = std::map<std::string, PeakCache>;
    void SetPeaks(const PeakMap* peaks) { fPeaks = peaks; Invalidate(); }

    // Move the playhead. Invalidates only the old + new columns, so the 60 Hz
    // poll doesn't repaint the whole view each tick.
    void SetPlayhead(Frame f);

    // Per-track output peaks (from the engine) for the header meters. Cleared
    // when playback stops. Keyed by TrackId -> (peakL, peakR) in [0, 1+].
    void SetTrackPeaks(const std::map<TrackId, std::pair<float, float>>& peaks) {
        fTrackPeaks = peaks; Invalidate();
    }
    void ClearTrackPeaks() { fTrackPeaks.clear(); Invalidate(); }

    // Live recording region drawn as a growing block on every armed track
    // while a take is being captured (before the real clips exist). Pass
    // active=false to clear it.
    void SetRecording(bool active, Frame start, Frame length);

private:
    void DrawRuler(BRect update);
    void DrawLanes(BRect update);
    // Iterate visible bar/beat gridlines: fn(x, isBar, barNumber).
    void ForEachGridLine(const std::function<void(float, bool, long)>& fn) const;
    void DrawTrackHeader(const Track& t, BRect lane);
    void DrawMidiNotes(const Track& t, BRect lane);
    void DrawClip(const Clip& c, BRect lane, rgb_color base);
    void DrawClipWave(const Clip& c, BRect block);
    void DrawPlayhead();
    void DrawDragGhost();   // clip-move preview rectangle

    // Lane geometry + header hit-testing.
    BRect LaneRect(int index) const;
    int   TrackIndexAt(BPoint where) const;             // -1 if none
    void  HandleHeaderClick(const Track& t, BRect lane, BPoint where);
    void  HandleRulerMenu(BPoint where);        // tempo/meter change menu
    Frame BarStartFrameAt(Frame f) const;       // nearest bar boundary frame

    // Live fader/pan drag. During a drag we preview by writing the model
    // directly; on release we restore the original and push ONE command, so
    // the whole gesture is a single clean undo step.
    enum class Drag { None, Gain, Pan, Clip, ClipResize, ClipFadeIn, ClipFadeOut,
                      ClipGain, Note, NoteResize, NoteVelocity, RulerLoop,
                      RulerPunch };
    void  PreviewDrag(BPoint where);   // apply the dragged value for feedback
    int   PitchAt(BRect lane, float y) const;   // y -> MIDI pitch
    Drag    fDrag      = Drag::None;
    TrackId fDragTrack = kInvalidTrackId;
    int     fDragLane  = -1;
    float   fDragOrig  = 0.0f;         // gain/pan value at drag start, for undo

    // Clip drag state.
    ClipId  fDragClip        = kInvalidClipId;
    bool    fDragIsMidiClip  = false;  // dragging a MIDI region (vs audio clip)
    Frame   fDragClipOrig    = 0;      // clip startFrame at drag start
    Frame   fDragClipOrigLen = 0;      // clip lengthFrames at drag start
    Frame   fDragGrabOffset  = 0;      // grabbed-frame - clip.startFrame
    Frame   fDragFadeInOrig  = 0;      // clip fades at drag start (for undo)
    Frame   fDragFadeOutOrig = 0;
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

    // Clipboard (right-click Copy -> Edit > Paste).
    int      ContextMenu(BPoint where, bool withSplit = false,
                         bool withTake = false) const;   // 0=Copy 1=Delete 2=Split 3=NextTake
    // Richer audio-clip menu (adds region ops); returns the chosen item's label
    // ("" if dismissed) so callers switch on meaning, not a fragile index.
    std::string AudioClipMenu(BPoint where, bool withTake) const;
    bool     PastePopup(BPoint where) const;    // "Paste here" -> true if chosen
    void     PasteToTrack(TrackId track, Frame at, TrackType type);
    bool     fHasClipClip = false;
    Clip     fClipClip;                // copied audio clip
    bool     fHasClipNote = false;
    MidiNote fClipNote;                // (legacy; unused)
    bool     fHasClipMidi = false;
    MidiClip fClipMidi;                // copied MIDI region
    TrackType fClipType = TrackType::Audio;  // source track type (paste target)

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

    std::set<ClipId> fSelClips;
    // Captured start of each selected MIDI region at drag-start, for group move.
    std::map<ClipId, Frame> fMidiMoveOrig;
    bool  ClipSelected(ClipId id) const { return fSelClips.count(id) != 0; }
    void  DeleteSelection();       // MacroCommand remove of all selected clips
    void  DuplicateSelection();    // MacroCommand add of offset copies
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
    Frame          fRecStart  = 0;
    Frame          fRecLen    = 0;
};

} // namespace daw
