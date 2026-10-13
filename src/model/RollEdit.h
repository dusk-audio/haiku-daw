// RollEdit — the piano roll's kit-free rules.
//
// The roll (src/ui/PianoRoll.cpp) is a Haiku-only view: it cannot be compiled
// or exercised on the host. Everything it decides that is not drawing lives
// here instead, so a host test covers it: what it must do when the model moves
// under its snapshot (the stale-list bug), whether a commit is still based on
// the state the stack is at (the same bug's other half), where a note's region
// window falls, how it snaps, which point a CC-lane click grabs, and how a lane
// address maps to a value.
//
// The rules mirror the model's own: `NoteInRegion` is the region-window rule of
// model/MidiOps.cpp and Track::CollectNotes (a note sounds iff its START is in
// [0, lengthFrames)), and `LaneValueDefault` is CcDefault/PitchBendAt's
// value-in-force fallback.
#pragma once

#include "MidiControl.h"
#include "MidiOps.h"       // NotesEqual
#include "Project.h"
#include "SnapGrid.h"
#include "TempoMap.h"
#include "types.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace daw {

// --- lane addressing -------------------------------------------------------
//
// The bottom strip is addressed as ONE lane number, the way the view stores it:
// -1 velocity, -2 pitch bend, 0..127 a controller. The bend lane is why the
// address is not simply "the CC number": a bend is a 14-bit stream of its own
// (MidiClipEvent::PitchBend), it sounds (PitchBendAt) and it is editable here.
constexpr int kLaneVelocity  = -1;
constexpr int kLanePitchBend = -2;

inline bool LaneIsValid(int lane) {
    return lane == kLaneVelocity || lane == kLanePitchBend
        || (lane >= 0 && lane <= 127);
}

// The highest value the lane's stream carries: 16383 for a 14-bit bend, 127
// for a controller. Velocity is a note field, not an event stream -- 127 like a
// controller.
inline int LaneValueMax(int lane) {
    return lane == kLanePitchBend ? 16383 : 127;
}

// The value in force where the lane's stream has no point yet: exactly what the
// engine renders there (CcDefault for a controller, the centre for a bend). A
// velocity lane has no stream of its own -- 100 is the velocity a note is
// created with, so "the default" is the same number the pencil uses.
inline int LaneValueDefault(int lane) {
    if (lane == kLanePitchBend) return 8192;
    if (lane == kLaneVelocity)  return 100;
    return CcDefault(lane);
}

// Does an event belong to this lane's stream? CC events are per controller
// (data); every other type is its own stream. The ONE definition of "the
// points this lane shows": the lane's draw, its hit test, its drag and its
// clear all read it, so they cannot disagree about what is on screen.
inline bool EventInLane(const MidiClipEvent& e, int lane) {
    if (lane == kLanePitchBend) return e.type == MidiClipEvent::PitchBend;
    if (lane < 0) return false;
    return e.type == MidiClipEvent::CC && e.data == lane;
}

// How many points the lane's stream carries.
inline int LanePointCount(const std::vector<MidiClipEvent>& events, int lane) {
    int n = 0;
    for (const MidiClipEvent& e : events) if (EventInLane(e, lane)) n++;
    return n;
}

// A lane value -> its position in the lane as a fraction of the lane's span
// (0 = the bottom line, 1 = the top). One mapping for the staircase, the
// handles and the drag, so a point is grabbed where it is drawn.
inline double LaneValueFraction(int lane, int value) {
    const int max = LaneValueMax(lane);
    if (value < 0) value = 0;
    if (value > max) value = max;
    return max > 0 ? (double)value / (double)max : 0.0;
}

// The inverse: a fraction of the span -> the lane's value (rounded, clamped).
inline int LaneValueAtFraction(int lane, double t) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    const int max = LaneValueMax(lane);
    return (int)(t * (double)max + 0.5);
}

// --- the region's window ---------------------------------------------------

// The region is a non-destructive window: a note plays iff its START lies in
// [0, clipLen). This must agree with model/MidiOps.cpp's InWindow and
// Track::CollectNotes -- the roll dims exactly what is silent, and dims nothing
// that sounds.
inline bool NoteInRegion(const MidiNote& n, Frame clipLen) {
    return n.startFrame >= 0 && n.startFrame < clipLen;
}

// How many of `notes` the region plays nothing of (the dimmed ones).
inline int NotesOutsideRegion(const std::vector<MidiNote>& notes, Frame clipLen) {
    int n = 0;
    for (const MidiNote& m : notes) if (!NoteInRegion(m, clipLen)) n++;
    return n;
}

// --- snapping ---------------------------------------------------------------

// Snap a CLIP-RELATIVE frame on the song's grid: the conversion to absolute
// frames, SnapFrame, and back, clamped into the region. The roll works
// clip-relative while the grid is the song's, so a region that starts off the
// bar still lines its notes up with the arrangement -- the behaviour the
// hard-coded 16th had, now with whatever grid the menu selects.
inline Frame SnapClipRelative(const TempoMap& tm, Frame clipRel, Frame clipStart,
                              SnapGrid grid) {
    Frame abs = clipRel + clipStart;
    if (abs < 0) abs = 0;
    const Frame out = SnapFrame(tm, abs, grid) - clipStart;
    return out < 0 ? 0 : out;
}

// --- the model ⇄ roll sync (the stale-list bug) -----------------------------

// One region's state: what the roll holds, or what the model has right now.
struct RollRegion {
    TrackId track  = kInvalidTrackId;
    ClipId  clip   = kInvalidClipId;
    Frame   start  = 0;      // region start, absolute frames
    Frame   length = 0;      // region length (the window's size)
    std::vector<MidiNote>      notes;    // clip-relative
    std::vector<MidiClipEvent> events;   // clip-relative
};

// What a roll must do to stay honest against the model.
enum class RollSync {
    Close,     // the region (or its track) is gone: close the editor
    Keep,      // nothing the roll holds disagrees with the model
    Repoint,   // only the region's start moved: re-point, keep the snapshot
    Reload,    // content or length changed: re-read, drop the selection
};

inline bool MidiEventsEqual(const std::vector<MidiClipEvent>& a,
                            const std::vector<MidiClipEvent>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (a[i].type != b[i].type || a[i].data != b[i].data
            || a[i].value != b[i].value || a[i].startFrame != b[i].startFrame)
            return false;
    return true;
}

// The decision, in order: a region that is gone closes the editor; content (or
// the window's length) that differs re-reads the whole snapshot; a region whose
// start alone moved only re-points (notes are clip-relative, so they are still
// the right notes -- and re-reading would throw away the selection for a move
// the user did in the arrange). A roll whose snapshot the model already agrees
// with is left completely alone.
inline RollSync DecideRollSync(const RollRegion& model, const RollRegion& roll) {
    if (model.track == kInvalidTrackId || model.clip == kInvalidClipId)
        return RollSync::Close;
    if (model.clip != roll.clip || model.track != roll.track)
        return RollSync::Close;    // a different region: not this roll's
    if (model.length != roll.length
        || !NotesEqual(model.notes, roll.notes)
        || !MidiEventsEqual(model.events, roll.events))
        return RollSync::Reload;
    if (model.start != roll.start)
        return RollSync::Repoint;
    return RollSync::Keep;
}

// The commit gate. Every edit the roll posts carries the stack serial it was
// based on; the main window applies it only while that serial is still the
// model's. A roll whose base is stale has missed a change (an undo, another
// view's edit) and its whole snapshot is suspect -- the fix is a reload, never
// to write the list on top of a model it never read.
inline bool RollCommitCurrent(uint64_t baseSerial, uint64_t modelSerial) {
    return baseSerial == modelSerial;
}

// ...but "the serial moved" is not the same as "someone else moved the model".
// TWO gestures can be posted inside one turn of the window's message loop (a
// fast click pair, a key burst), and the second carries the SAME base: the push
// that would raise it is only sent while the first is being handled. Refusing
// it would silently drop the user's second gesture. So the window remembers the
// transition its last ACCEPTED roll commit made — the base it named, and the
// serial that commit left the stack at — and a commit is also allowed while the
// model is still standing exactly where the roll's own commit left it. Any
// other change (an undo, another view's edit) leaves that state, and the gate
// is strict again.
struct RollCommitGate {
    uint64_t base   = 0;   // the serial the last accepted commit named
    uint64_t result = 0;   // ... and the serial it left the stack at
};

inline bool RollCommitAllowed(uint64_t baseSerial, uint64_t modelSerial,
                              const RollCommitGate& own) {
    if (RollCommitCurrent(baseSerial, modelSerial)) return true;
    return baseSerial == own.base && modelSerial == own.result;
}

inline void RollCommitAccepted(RollCommitGate& own, uint64_t baseSerial,
                               uint64_t modelSerial) {
    own.base   = baseSerial;
    own.result = modelSerial;
}

// --- selection edits (nudge, duplicate, copy) -------------------------------

// One note's geometry before a drag: the group move/resize applies ONE delta
// from these, so a gesture cannot accumulate drift.
struct NoteGeometry {
    Frame start = 0;
    Frame len   = 0;
    int   pitch = 0;
    int   velocity = 0;
};

inline std::vector<NoteGeometry> CaptureGeometry(const std::vector<MidiNote>& notes) {
    std::vector<NoteGeometry> out;
    out.reserve(notes.size());
    for (const MidiNote& n : notes)
        out.push_back(NoteGeometry{ n.startFrame, n.lengthFrames, n.pitch,
                                    n.velocity });
    return out;
}

// Move every selected note by one delta from its captured origin. Starts clamp
// at 0 (the region's head), pitches clamp to 0..127. Returns how many moved.
inline int NudgeSelection(std::vector<MidiNote>& notes, const std::vector<char>& sel,
                          const std::vector<NoteGeometry>& orig,
                          Frame dFrames, int dPitches) {
    if (orig.size() != notes.size() || sel.size() != notes.size()) return 0;
    int moved = 0;
    for (std::size_t i = 0; i < notes.size(); i++) {
        if (!sel[i]) continue;
        Frame s = orig[i].start + dFrames;
        if (s < 0) s = 0;
        const int p = std::clamp(orig[i].pitch + dPitches, 0, 127);
        if (s != notes[i].startFrame || p != notes[i].pitch) moved++;
        notes[i].startFrame = s;
        notes[i].pitch      = p;
    }
    return moved;
}

// The copies of every selected note, offset by `dFrames` (each copy's pitch
// unchanged), in list order. The CALLER appends them and re-selects: the roll
// does it in one place so the mask stays index-aligned with the list.
inline std::vector<MidiNote> CopySelection(const std::vector<MidiNote>& notes,
                                           const std::vector<char>& sel,
                                           Frame dFrames) {
    std::vector<MidiNote> out;
    for (std::size_t i = 0; i < notes.size() && i < sel.size(); i++) {
        if (!sel[i]) continue;
        MidiNote n = notes[i];
        n.startFrame = n.startFrame + dFrames;
        if (n.startFrame < 0) n.startFrame = 0;
        out.push_back(n);
    }
    return out;
}

// Append `copies` to `notes` and leave exactly them selected. Empty selection or
// no copies: untouched, and the caller can tell (the returned count).
inline int AppendSelecting(std::vector<MidiNote>& notes, std::vector<char>& sel,
                           std::vector<MidiNote> copies) {
    if (copies.empty()) return 0;
    const int n = (int)copies.size();
    sel.assign(notes.size(), 0);
    for (MidiNote& c : copies) {
        notes.push_back(c);
        sel.push_back(1);
    }
    return n;
}

// --- the CC lane's points ---------------------------------------------------

// The point of the lane's stream nearest to `frame`, within `tol` frames
// (the caller converts its pixel grab radius with the zoom), or -1.
inline int CcPointNearFrame(const std::vector<MidiClipEvent>& events, int lane,
                            Frame frame, Frame tol) {
    int best = -1;
    Frame bestD = tol;
    for (std::size_t i = 0; i < events.size(); i++) {
        if (!EventInLane(events[i], lane)) continue;
        const Frame d = events[i].startFrame - frame;
        const Frame ad = d < 0 ? -d : d;
        if (ad <= bestD) { bestD = ad; best = (int)i; }   // ties: the later one
    }
    return best;
}

// Insert, or replace the value of the point already at that frame: one point
// per stream per frame -- a step function evaluates the latest at-or-before, so
// a second point at the same frame would be dead weight the lane draws twice.
inline void CcPointSet(std::vector<MidiClipEvent>& events, int lane, Frame frame,
                       int value) {
    const int max = LaneValueMax(lane);
    if (value < 0) value = 0;
    if (value > max) value = max;
    for (MidiClipEvent& e : events)
        if (EventInLane(e, lane) && e.startFrame == frame) { e.value = value; return; }
    MidiClipEvent e;
    e.type = (lane == kLanePitchBend) ? MidiClipEvent::PitchBend
                                      : MidiClipEvent::CC;
    e.data = lane >= 0 ? lane : 0;
    e.startFrame = frame;
    e.value = value;
    events.push_back(e);
}

// Move the point at `index` to a new frame and value. The index IS the drag's
// identity (the vector is not re-sorted while dragging), so a moved point
// leaves nothing behind -- the bug the old "write a point at every new x" path
// had. Another point of the same stream already at `frame` is absorbed.
// Returns false for an index that names nothing in this lane.
inline bool CcPointMove(std::vector<MidiClipEvent>& events, int lane, int index,
                        Frame frame, int value) {
    if (index < 0 || (std::size_t)index >= events.size()) return false;
    if (!EventInLane(events[(std::size_t)index], lane)) return false;
    if (frame < 0) frame = 0;
    const int max = LaneValueMax(lane);
    if (value < 0) value = 0;
    if (value > max) value = max;
    // Absorb every other point of this stream already at the target frame.
    // The moving point's own position is tracked as elements go, so a point
    // erased BELOW it cannot shift the write onto the wrong event (the first
    // cut erased without adjusting and wrote out of bounds — the host test
    // caught it).
    int at = index;
    for (std::size_t i = 0; i < events.size();) {
        if ((int)i != at && EventInLane(events[i], lane)
            && events[i].startFrame == frame) {
            events.erase(events.begin() + (long)i);
            if ((int)i < at) at--;
            continue;                       // the next event now sits at i
        }
        i++;
    }
    events[(std::size_t)at].startFrame = frame;
    events[(std::size_t)at].value = value;
    return true;
}

// Remove every point of one lane's stream; returns how many went (the caller
// skips the undo step when it is 0).
inline int CcLaneClear(std::vector<MidiClipEvent>& events, int lane) {
    int removed = 0;
    for (std::size_t i = 0; i < events.size();) {
        if (!EventInLane(events[i], lane)) { i++; continue; }
        events.erase(events.begin() + (long)i);
        removed++;
    }
    return removed;
}

// --- the toolbar's layout ---------------------------------------------------
//
// The editor's toolbar is drawn, so its geometry is a rule like any other: a
// kit-free layout keeps the drawing, the hit test and the tooltips on one
// answer, and lets a host test pin that a narrow dock still reaches MIDI.
// Design-pixel offsets; the caller scales them through Themed().
constexpr float kRollToolX     = 4.0f;
constexpr float kRollToolPitch = 26.0f;
constexpr float kRollToolW     = 22.0f;
constexpr float kRollToolTop   = 5.0f;
constexpr float kRollToolBot   = 23.0f;
constexpr float kRollToolGap   = 8.0f;
constexpr float kRollGridW     = 56.0f;
constexpr float kRollZoomW     = 24.0f;
constexpr float kRollMidiW     = 56.0f;
constexpr float kRollMidiMinW  = 28.0f;

struct RollToolbarLayout {
    float gridX    = 0.0f;
    float zoomOutX = 0.0f;
    float zoomInX  = 0.0f;
    float midiX    = 0.0f;
    float midiW    = kRollMidiW;   // 0 = no room: draw none, hit none
};

// Lay the toolbar's right-hand group out for a view `width` design pixels
// wide, with room for `tools` tool buttons. The tools come first; the buttons
// that follow keep their width until the MIDI button alone would not fit, and
// its width then shrinks (never below kMidiMinW) so the menu stays reachable in
// a dock narrower than its own toolbar. A view too narrow for even that reports
// midiW 0 -- the caller draws no MIDI button, and its hit test agrees because
// both read this.
inline RollToolbarLayout RollToolbarFor(float width, int tools) {
    RollToolbarLayout l;
    float x = kRollToolX + (float)tools * kRollToolPitch + kRollToolGap;
    l.gridX    = x; x += kRollGridW + kRollToolGap;
    l.zoomOutX = x; x += kRollZoomW + 4.0f;
    l.zoomInX  = x; x += kRollZoomW + kRollToolGap;
    l.midiX    = x;
    const float room = width - 2.0f - l.midiX;
    if (room < kRollMidiMinW)   l.midiW = 0.0f;
    else if (room < kRollMidiW) l.midiW = room;
    return l;
}

} // namespace daw
