// Host tests for RollEdit: the piano roll's kit-free rules — when its snapshot
// has gone stale (the stale-list bug), when a commit may be applied, the region
// window the drawing and the engine must agree on, the snap through the song's
// grid, the lane addressing (including pitch bend) and the CC lane's point
// editing (drag identity, absorb, clear) and toolbar layout.
//
//   g++ -std=c++17 -Isrc tests/rolledit_tests.cpp src/model/MidiOps.cpp -o /tmp/re && /tmp/re

#include "../src/model/RollEdit.h"

#include <cstdio>
#include <cstring>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static MidiNote Note(int pitch, Frame start, Frame len = 1000, int vel = 100) {
    MidiNote n; n.pitch = pitch; n.startFrame = start; n.lengthFrames = len;
    n.velocity = vel; return n;
}
static MidiClipEvent Cc(int cc, Frame at, int v) {
    MidiClipEvent e; e.type = MidiClipEvent::CC; e.data = cc;
    e.startFrame = at; e.value = v; return e;
}
static MidiClipEvent Bend(Frame at, int v) {
    MidiClipEvent e; e.type = MidiClipEvent::PitchBend;
    e.startFrame = at; e.value = v; return e;
}

static RollRegion Region(TrackId t, ClipId c, Frame start, Frame len,
                         std::vector<MidiNote> notes = {},
                         std::vector<MidiClipEvent> events = {}) {
    RollRegion r;
    r.track = t; r.clip = c; r.start = start; r.length = len;
    r.notes = std::move(notes); r.events = std::move(events);
    return r;
}

int main() {
    const double SR = 48000.0;
    TempoMap tm;
    tm.sampleRate = SR;   // 120 BPM: 24000 frames per beat

    // --- the sync decision -------------------------------------------------
    {
        const TrackId T = 7; const ClipId C = 9;
        RollRegion model = Region(T, C, 48000, 96000, { Note(60, 0), Note(64, 6000) });

        // Agreeing snapshots stay put: no needless rebuilds (and no cancelled
        // gestures) on every 2 Hz poll.
        CHECK(DecideRollSync(model, model) == RollSync::Keep);

        // The region moved: the notes are still the right notes (clip-relative),
        // so only the origin changes.
        RollRegion roll = model;
        model.start = 96000;
        CHECK(DecideRollSync(model, roll) == RollSync::Repoint);
        model = Region(T, C, 48000, 96000, { Note(60, 0), Note(64, 6000) });

        // The notes changed under the roll (an undo, another view's edit).
        roll.notes.push_back(Note(67, 12000));
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        roll = model;

        // The length changed (a split trims the half): reload, or the next
        // gesture grows the region back to the old length.
        roll.length = 48000;
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        roll = model;

        // The events changed: reload (the CC lane's snapshot is stale too).
        roll.events.push_back(Cc(1, 0, 64));
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        roll = model;

        // A note's velocity alone is still a different note list.
        roll.notes[0].velocity = 90;
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        roll = model;
        roll.notes[0].lengthFrames = 500;
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        roll = model;

        // A length difference outranks a start difference: reload wins.
        roll.length = 1; model.start = 96000;
        CHECK(DecideRollSync(model, roll) == RollSync::Reload);
        model = Region(T, C, 48000, 96000, { Note(60, 0), Note(64, 6000) });

        // The region is gone (or the track is): close. Either half alone is
        // enough -- a track that vanished takes its region with it.
        RollRegion gone;
        CHECK(DecideRollSync(gone, roll) == RollSync::Close);
        CHECK(DecideRollSync(Region(kInvalidTrackId, C, 0, 0), roll)
              == RollSync::Close);
        CHECK(DecideRollSync(Region(T, kInvalidClipId, 0, 0), roll)
              == RollSync::Close);
        // A DIFFERENT region's state is not this roll's business: close rather
        // than reload someone else's notes into it.
        CHECK(DecideRollSync(Region(T, 11, 0, 100), roll) == RollSync::Close);
    }

    // --- the commit gate ---------------------------------------------------
    {
        CHECK(RollCommitCurrent(5, 5));
        CHECK(!RollCommitCurrent(5, 6));    // the model moved on
        CHECK(!RollCommitCurrent(0, 1));
        CHECK(RollCommitCurrent(0, 0));     // the empty history is a state too
    }
    {
        // A fresh gate has no own-transition: only an exact match passes.
        RollCommitGate g;
        CHECK(RollCommitAllowed(7, 7, g));
        CHECK(!RollCommitAllowed(7, 8, g));
        // After the roll's own commit (7 -> 8), a SECOND commit posted in the
        // same batch (it still names 7, and no push has reached it) is applied
        // rather than silently dropped -- the model is exactly where that
        // commit left it.
        RollCommitAccepted(g, 7, 8);
        CHECK(RollCommitAllowed(7, 8, g));
        // ... but only while it is: an undo (8 -> 9) ends the grace, and the
        // batched commit would then be writing over a state it never saw.
        CHECK(!RollCommitAllowed(7, 9, g));
        CHECK(RollCommitAllowed(9, 9, g));   // a re-synced roll is fine again
        // A different base is never waved through by the historical transition.
        CHECK(!RollCommitAllowed(6, 8, g));
        // Two accepted commits in a row: the transition moves with them.
        RollCommitAccepted(g, 9, 10);
        CHECK(RollCommitAllowed(9, 10, g));
        CHECK(!RollCommitAllowed(7, 10, g));   // the older base is dead
    }

    // --- the region's window ------------------------------------------------
    {
        const Frame len = 96000;
        CHECK(NoteInRegion(Note(60, 0), len));            // the head sounds
        CHECK(NoteInRegion(Note(60, len - 1), len));      // the last frame too
        CHECK(!NoteInRegion(Note(60, len), len));         // the end is outside
        CHECK(!NoteInRegion(Note(60, len + 5000), len));  // past the end
        CHECK(!NoteInRegion(Note(60, -1), len));          // trimmed off the front
        // Length does not matter: a note STARTING inside sounds even when it
        // runs past the end (the engine cuts nothing off the start rule).
        CHECK(NoteInRegion(Note(60, len - 100, 100000), len));
        std::vector<MidiNote> notes = { Note(60, 0), Note(60, len),
                                        Note(60, len + 1), Note(60, 10) };
        CHECK(NotesOutsideRegion(notes, len) == 2);
        CHECK(NotesOutsideRegion({}, len) == 0);
    }

    // --- snapping in the region's own space ---------------------------------
    {
        // A region starting at bar 3 (48000, two beats in at 120 BPM): a
        // clip-relative frame snaps to the SONG's grid, not to the region head.
        const Frame clipStart = 48000;
        SnapGrid g{ SnapKind::Sixteenth, false };     // 6000-frame steps
        CHECK(SnapClipRelative(tm, 0, clipStart, g) == 0);
        CHECK(SnapClipRelative(tm, 2000, clipStart, g) == 0);      // -> bar line
        CHECK(SnapClipRelative(tm, 4000, clipStart, g) == 6000);   // -> next 16th
        // Triplets: one step = a third of a beat (8000 frames) — the grid
        // point the region head sits on at 48000 is itself one of them, so the
        // clip-relative lines fall at 0, 8000, 16000.
        SnapGrid tri{ SnapKind::Eighth, true };
        CHECK(SnapClipRelative(tm, 3000, clipStart, tri) == 0);
        CHECK(SnapClipRelative(tm, 5000, clipStart, tri) == 8000);
        CHECK(SnapClipRelative(tm, 9000, clipStart, tri) == 8000);
        CHECK(SnapClipRelative(tm, 15000, clipStart, tri) == 16000);
        // Off is the identity of the absolute frame (the free-placement path is
        // the caller's Shift check, but Off must not move anything either).
        SnapGrid off{ SnapKind::Off, false };
        CHECK(SnapClipRelative(tm, 4711, clipStart, off) == 4711);
        // A region whose head is off the grid: a frame that snaps in front of
        // the head clamps to the head itself (0), one on a line lands on it.
        CHECK(SnapClipRelative(tm, 1000, 1000, g) == 0);
        CHECK(SnapClipRelative(tm, 5000, 1000, g) == 5000);
        // Never negative: a frame before the song start snaps to 0, and the
        // region-relative result clamps at 0 too.
        CHECK(SnapClipRelative(tm, -30000, clipStart, g) == 0);
        CHECK(SnapClipRelative(tm, -1, 0, g) == 0);
    }

    // --- lane addressing ----------------------------------------------------
    {
        CHECK(LaneIsValid(kLaneVelocity));
        CHECK(LaneIsValid(kLanePitchBend));
        CHECK(LaneIsValid(0));
        CHECK(LaneIsValid(127));
        CHECK(!LaneIsValid(-3));
        CHECK(!LaneIsValid(128));

        CHECK(LaneValueMax(kLanePitchBend) == 16383);
        CHECK(LaneValueMax(1) == 127);
        CHECK(LaneValueMax(kLaneVelocity) == 127);
        CHECK(LaneValueDefault(kLanePitchBend) == 8192);   // centred, as the engine
        CHECK(LaneValueDefault(7) == 127);                 // CcDefault: unity
        CHECK(LaneValueDefault(10) == 64);                 // CcDefault: centre
        CHECK(LaneValueDefault(74) == 0);

        // The lane's stream: CC events are per controller, a bend is its own.
        CHECK(EventInLane(Cc(1, 0, 5), 1));
        CHECK(!EventInLane(Cc(2, 0, 5), 1));
        CHECK(EventInLane(Bend(0, 8192), kLanePitchBend));
        CHECK(!EventInLane(Cc(1, 0, 5), kLanePitchBend));
        CHECK(!EventInLane(Bend(0, 8192), kLaneVelocity));
        std::vector<MidiClipEvent> ev = { Cc(1, 0, 5), Cc(2, 0, 5), Bend(10, 8192),
                                          Cc(1, 100, 6) };
        CHECK(LanePointCount(ev, 1) == 2);
        CHECK(LanePointCount(ev, 2) == 1);
        CHECK(LanePointCount(ev, kLanePitchBend) == 1);
        CHECK(LanePointCount(ev, kLaneVelocity) == 0);

        // Fraction <-> value round trips, at both ends and the middle.
        CHECK(LaneValueFraction(1, 0) == 0.0);
        CHECK(LaneValueFraction(1, 127) == 1.0);
        CHECK(LaneValueFraction(kLanePitchBend, 8192) > 0.4999);
        CHECK(LaneValueFraction(kLanePitchBend, 8192) < 0.5001);
        CHECK(LaneValueAtFraction(1, 0.0) == 0);
        CHECK(LaneValueAtFraction(1, 1.0) == 127);
        CHECK(LaneValueAtFraction(kLanePitchBend, 0.5) == 8192);
        CHECK(LaneValueAtFraction(kLanePitchBend, 1.0) == 16383);
        CHECK(LaneValueAtFraction(1, -1.0) == 0);       // clamped
        CHECK(LaneValueAtFraction(1, 2.0) == 127);
    }

    // --- CC lane: hit, move, absorb, clear ----------------------------------
    {
        std::vector<MidiClipEvent> ev = { Cc(1, 1000, 20), Cc(1, 5000, 60),
                                          Cc(2, 3000, 7) };
        // The hit test finds the lane's own stream only, within tolerance.
        CHECK(CcPointNearFrame(ev, 1, 1000, 500) == 0);
        CHECK(CcPointNearFrame(ev, 1, 1200, 500) == 0);     // within tol
        CHECK(CcPointNearFrame(ev, 1, 1600, 500) == -1);    // past tol
        CHECK(CcPointNearFrame(ev, 1, 3000, 500) == -1);    // CC2 is not this lane
        CHECK(CcPointNearFrame(ev, 2, 3000, 500) == 2);
        CHECK(CcPointNearFrame(ev, kLanePitchBend, 1000, 500) == -1);
        CHECK(CcPointNearFrame(ev, 1, 5000, 500) == 1);

        // A drag MOVES the grabbed point: the bug was that the drag wrote a new
        // point at every x and left the old one behind.
        CHECK(CcPointMove(ev, 1, 0, 2000, 100));
        CHECK(ev.size() == 3);                     // nothing added
        CHECK(ev[0].startFrame == 2000);
        CHECK(ev[0].value == 100);
        CHECK(CcPointMove(ev, 1, 0, 4500, 127));   // still the same point
        CHECK(ev.size() == 3);
        CHECK(ev[0].startFrame == 4500);
        // Clamps, not corruption.
        CHECK(CcPointMove(ev, 1, 0, -10, 999));
        CHECK(ev[0].startFrame == 0);
        CHECK(ev[0].value == 127);
        // The index must name a point OF THIS LANE: a wrong lane or a stale
        // index is refused, never silently written to another stream.
        CHECK(!CcPointMove(ev, 1, 2, 100, 100));       // index 2 is the CC2 point
        CHECK(!CcPointMove(ev, 1, 99, 100, 100));
        CHECK(!CcPointMove(ev, kLanePitchBend, 0, 100, 100));

        // Moving onto a frame that already holds a point of the same stream
        // absorbs it: one point per stream per frame (the step function's rule).
        {
            std::vector<MidiClipEvent> two = { Cc(1, 1000, 10), Cc(1, 3000, 20) };
            CHECK(CcPointMove(two, 1, 0, 3000, 30));
            CHECK(two.size() == 1);
            CHECK(two[0].startFrame == 3000);
            CHECK(two[0].value == 30);
        }
        // And the same for a bend moved onto a bend.
        {
            std::vector<MidiClipEvent> b = { Bend(1000, 8192), Bend(3000, 9000) };
            CHECK(CcPointMove(b, kLanePitchBend, 1, 1000, 4000));
            CHECK(b.size() == 1);
            CHECK(b[0].startFrame == 1000);
            CHECK(b[0].value == 4000);
        }

        // The ramp path: insert, then update in place; a bend keeps its type.
        {
            std::vector<MidiClipEvent> r;
            CcPointSet(r, 1, 1000, 50);
            CHECK(r.size() == 1);
            CHECK(r[0].type == MidiClipEvent::CC && r[0].data == 1);
            CcPointSet(r, 1, 1000, 90);
            CHECK(r.size() == 1);
            CHECK(r[0].value == 90);
            CcPointSet(r, 1, 1000, 999);           // clamped to the CC max
            CHECK(r[0].value == 127);
            CcPointSet(r, kLanePitchBend, 2000, 16383);
            CHECK(r.size() == 2);
            CHECK(r[1].type == MidiClipEvent::PitchBend);
            CcPointSet(r, kLanePitchBend, 2000, 99999);   // the bend's own max
            CHECK(r[1].value == 16383);
        }

        // Clear lane removes one stream and leaves the others exactly as they
        // were (including another controller's points).
        {
            std::vector<MidiClipEvent> c = { Cc(1, 0, 5), Cc(2, 0, 5),
                                             Cc(1, 10, 6), Bend(0, 1) };
            CHECK(CcLaneClear(c, 1) == 2);
            CHECK(c.size() == 2);
            CHECK(c[0].data == 2);
            CHECK(c[1].type == MidiClipEvent::PitchBend);
            CHECK(CcLaneClear(c, 1) == 0);        // nothing left: no undo step
            CHECK(CcLaneClear(c, kLanePitchBend) == 1);
            CHECK(c.size() == 1);
        }
    }

    // --- selection edits ----------------------------------------------------
    {
        std::vector<MidiNote> notes = { Note(60, 1000), Note(62, 2000),
                                        Note(64, 3000) };
        std::vector<char> sel = { 1, 0, 1 };
        std::vector<NoteGeometry> orig = CaptureGeometry(notes);
        CHECK(NudgeSelection(notes, sel, orig, 6000, 1) == 2);
        CHECK(notes[0].startFrame == 7000 && notes[0].pitch == 61);
        CHECK(notes[1].startFrame == 2000 && notes[1].pitch == 62);  // untouched
        CHECK(notes[2].startFrame == 9000 && notes[2].pitch == 65);

        // Clamped at the head and at the pitch ends, one delta from the origin
        // (a second nudge must not stack on the first).
        std::vector<MidiNote> low = { Note(0, 100), Note(127, 100) };
        std::vector<char> all = { 1, 1 };
        std::vector<NoteGeometry> lo = CaptureGeometry(low);
        CHECK(NudgeSelection(low, all, lo, -500, -1) == 2);
        CHECK(low[0].startFrame == 0 && low[0].pitch == 0);
        CHECK(low[1].startFrame == 0 && low[1].pitch == 126);
        // The same gesture applied twice from the same origin moves nothing the
        // second time: the delta is from the capture, never cumulative.
        CHECK(NudgeSelection(low, all, lo, -500, -1) == 0);
        CHECK(low[0].startFrame == 0 && low[0].pitch == 0);
        // A mask of the wrong size is refused outright (never a half-move).
        std::vector<char> bad = { 1 };
        CHECK(NudgeSelection(low, bad, lo, 10, 0) == 0);

        // Duplicate/copy: the copies are the selection, appended.
        std::vector<MidiNote> src = { Note(60, 1000), Note(62, 2000) };
        std::vector<char> pick = { 0, 1 };
        std::vector<MidiNote> cp = CopySelection(src, pick, 6000);
        CHECK(cp.size() == 1);
        CHECK(cp[0].pitch == 62 && cp[0].startFrame == 8000);
        std::vector<MidiNote> dst = src;
        std::vector<char> dsel = pick;
        CHECK(AppendSelecting(dst, dsel, cp) == 1);
        CHECK(dst.size() == 3);
        CHECK(dsel.size() == 3);
        CHECK(dsel[0] == 0 && dsel[1] == 0 && dsel[2] == 1);
        CHECK(AppendSelecting(dst, dsel, {}) == 0);   // no copies: untouched
        CHECK(dst.size() == 3);
        // A copy pushed before the head clamps at 0 rather than going negative.
        std::vector<MidiNote> near = { Note(60, 500) };
        std::vector<char> one = { 1 };
        std::vector<MidiNote> cp2 = CopySelection(near, one, -2000);
        CHECK(cp2.size() == 1 && cp2[0].startFrame == 0);
    }

    // --- toolbar layout -----------------------------------------------------
    {
        // A wide dock: every button keeps its design width, MIDI last.
        RollToolbarLayout w = RollToolbarFor(900.0f, 7);
        CHECK(w.midiW == kRollMidiW);
        CHECK(w.gridX > 7 * kRollToolPitch);
        CHECK(w.midiX > w.zoomInX);
        // A dock narrower than the toolbar: MIDI shrinks but stays reachable.
        RollToolbarLayout n = RollToolbarFor(w.midiX + 40.0f, 7);
        CHECK(n.midiW > 0.0f);
        CHECK(n.midiW < kRollMidiW);
        CHECK(n.midiX + n.midiW <= w.midiX + 40.0f);
        // Too narrow for even the minimum: no MIDI button (and the hit test,
        // which reads the same layout, agrees there is nothing there).
        RollToolbarLayout t = RollToolbarFor(w.midiX + 8.0f, 7);
        CHECK(t.midiW == 0.0f);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
