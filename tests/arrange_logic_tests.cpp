// Host tests for the arrangement view's kit-free rules (src/model/ArrangeLogic.h)
// and the marker move command the marker lane's drag runs through the stack.
//
// The four rules come from real defects in TimelineView (package
// 50-ux-timeline): a crossfade overlap clicked the clip UNDERNEATH, the Loop
// button lit with a zero-length loop, the scrollbar's arrows moved a fiftieth
// of a pixel, and Delete acted on a stale time range instead of the clip the
// user had just clicked.
//
//   g++ -std=c++17 -Isrc tests/arrange_logic_tests.cpp -o /tmp/al && /tmp/al

#include "../src/model/ArrangeLogic.h"
#include "../src/model/Commands.h"

#include <cstdio>
#include <memory>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// The two real item types, so the rule is tested against the fields the view
// actually hands it — not a stand-in that could drift from them.
static Clip AudioClip(ClipId id, Frame start, Frame len) {
    Clip c;
    c.id = id;
    c.startFrame = start;
    c.lengthFrames = len;
    return c;
}
static const auto kDrawAll = [](const Clip&) { return false; };

// --- the draw-order hit rule ------------------------------------------------
static void test_topmost_clip() {
    std::printf("topmost clip\n");
    // Two clips that overlap in a crossfade: A starts earlier, B overlaps its
    // tail and is drawn after it, so B is on top over [B.start, A.end).
    std::vector<Clip> clips = { AudioClip(1, 1000, 4000),    // 1000..5000
                                AudioClip(2, 4000, 4000) };  // 4000..8000

    // Outside every span: nothing.
    CHECK(TopmostClipIndexAt(clips, 0, kDrawAll) == -1);
    CHECK(TopmostClipIndexAt(clips, 999, kDrawAll) == -1);
    CHECK(TopmostClipIndexAt(clips, 8000, kDrawAll) == -1);   // end is exclusive

    // A's own span: A.
    CHECK(TopmostClipIndexAt(clips, 1000, kDrawAll) == 0);    // start inclusive
    CHECK(TopmostClipIndexAt(clips, 3999, kDrawAll) == 0);

    // The overlap: the LATER clip — the one drawn on top. (The defect: this
    // returned 0, so the click edited the clip under the crossfade.)
    CHECK(TopmostClipIndexAt(clips, 4000, kDrawAll) == 1);
    CHECK(TopmostClipIndexAt(clips, 4999, kDrawAll) == 1);

    // B alone.
    CHECK(TopmostClipIndexAt(clips, 5000, kDrawAll) == 1);
    CHECK(TopmostClipIndexAt(clips, 7999, kDrawAll) == 1);

    // Reversed vector order (nothing guarantees the caller sorted it): the
    // rule is "greatest start wins", not "later index wins" — the greater
    // start is still the clip drawn last.
    std::vector<Clip> unsorted = { AudioClip(2, 4000, 4000),   // B first
                                   AudioClip(1, 1000, 4000) };
    CHECK(TopmostClipIndexAt(unsorted, 4000, kDrawAll) == 0);  // B (start 4000)

    // Equal starts: the LATER index is the one drawn last, so it wins.
    std::vector<Clip> same = { AudioClip(1, 2000, 2000),
                               AudioClip(2, 2000, 2000) };
    CHECK(TopmostClipIndexAt(same, 2500, kDrawAll) == 1);

    // A hidden take is skipped, and skipping it does not change which of the
    // visible ones is on top.
    std::vector<Clip> takes = { AudioClip(1, 1000, 4000),
                                AudioClip(2, 4000, 4000),
                                AudioClip(3, 4500, 500) };
    takes[2].takeGroup = 1;
    takes[2].takeActive = false;          // hidden: not a hit target
    auto skip = [](const Clip& c) { return c.takeGroup > 0 && !c.takeActive; };
    CHECK(TopmostClipIndexAt(takes, 4600, skip) == 1);   // B, not the hidden one
    takes[2].takeActive = true;
    CHECK(TopmostClipIndexAt(takes, 4600, skip) == 2);   // shown, and on top

    // The MIDI region path uses the same rule through the same template.
    std::vector<MidiClip> regions(2);
    regions[0].id = 1; regions[0].startFrame = 0;    regions[0].lengthFrames = 1000;
    regions[1].id = 2; regions[1].startFrame = 800;  regions[1].lengthFrames = 1000;
    auto mskip = [](const MidiClip& c) { return c.takeGroup > 0 && !c.takeActive; };
    CHECK(TopmostClipIndexAt(regions, 900, mskip) == 1);
    CHECK(TopmostClipIndexAt(regions, 100, mskip) == 0);

    // A zero-length block can never contain anything.
    std::vector<Clip> empty = { AudioClip(1, 1000, 0) };
    CHECK(TopmostClipIndexAt(empty, 1000, kDrawAll) == -1);
}

// --- the loop range the L key adopts ---------------------------------------
static void test_default_loop_range() {
    std::printf("default loop range\n");
    TempoMap tm;
    tm.sampleRate = 48000.0;      // 120 BPM -> 24000 frames per beat, 96000/bar
    Frame lo = -1, hi = -1;

    // Mid-bar 1: the bar containing the playhead.
    CHECK(DefaultLoopRange(tm, 10000, &lo, &hi));
    CHECK(lo == 0);
    CHECK(hi == 96000);
    CHECK(hi > lo);                        // never a zero-length loop

    // Exactly on the bar line: that bar, not the previous one.
    CHECK(DefaultLoopRange(tm, 96000, &lo, &hi));
    CHECK(lo == 96000);
    CHECK(hi == 192000);

    // Late in a bar: still that bar.
    CHECK(DefaultLoopRange(tm, 96000 + 95999, &lo, &hi));
    CHECK(lo == 96000);
    CHECK(hi == 192000);

    // A negative playhead clamps to the top of the project rather than
    // producing a range before frame 0.
    CHECK(DefaultLoopRange(tm, -5, &lo, &hi));
    CHECK(lo == 0);
    CHECK(hi == 96000);

    // A meter change moves the bars with it: 3/4 from frame 0 (72000 frames a
    // bar at 120 BPM), then 7/8 from frame 144000 (which is bar 3's line in
    // the 3/4 grid — meter changes are expected to land on a bar).
    TempoMap m2;
    m2.sampleRate = 48000.0;
    m2.SetMeterAt(0, 3, 4);
    m2.SetMeterAt(144000, 7, 8);
    CHECK(DefaultLoopRange(m2, 0, &lo, &hi));
    CHECK(lo == 0);
    CHECK(hi == 72000);
    CHECK(DefaultLoopRange(m2, 50000, &lo, &hi));    // still inside bar 1
    CHECK(lo == 0);
    CHECK(hi == 72000);
    CHECK(DefaultLoopRange(m2, 72000, &lo, &hi));    // the 3/4 segment's bar 2
    CHECK(lo == 72000);
    CHECK(hi == 144000);
    CHECK(DefaultLoopRange(m2, 144000, &lo, &hi));   // the first 7/8 bar
    CHECK(lo == 144000);
    CHECK(hi == 228000);                             // 3.5 beats at 24000 f/beat

    // A tempo so fast that a bar rounds to zero frames is refused (the caller
    // then says so and leaves the loop lamp off) rather than handing back a
    // zero-length loop.
    TempoMap broken;
    broken.sampleRate = 48000.0;
    broken.SetTempoAt(0, 1.0e9);
    CHECK(!DefaultLoopRange(broken, 1000, &lo, &hi));
}

// --- the scrollbar steps ----------------------------------------------------
static void test_scroll_steps() {
    std::printf("scrollbar steps\n");
    float small = 0.0f, large = 0.0f;

    // Default zoom (512 frames/px) with a 1000 px content area.
    ScrollStepsFor(512.0, 1000.0f, &small, &large);
    CHECK(small == 512.0f * 16.0f);        // one 16 px nudge
    CHECK(large == 512.0f * 1000.0f);      // one screenful
    // The defect, stated as a check: the old stock step (12 frames) was a
    // fiftieth of a pixel, i.e. invisible.
    CHECK(small / 512.0 > 1.0);            // the arrow moves >1 px of content

    // Zoomed in: the steps shrink with the zoom, still 16 px / one screen.
    ScrollStepsFor(48.0, 640.0f, &small, &large);
    CHECK(small == 48.0f * 16.0f);
    CHECK(large == 48.0f * 640.0f);

    // Never zero: a step below one frame would be a no-op.
    ScrollStepsFor(0.01, 0.4f, &small, &large);
    CHECK(small >= 1.0f);
    CHECK(large >= 1.0f);
}

// --- which selection Delete acts on ----------------------------------------
static void test_selection_target() {
    std::printf("selection target\n");
    CHECK(SelectionTargetFor(false, false) == SelectTarget::None);
    CHECK(SelectionTargetFor(true,  false) == SelectTarget::Range);
    CHECK(SelectionTargetFor(false, true)  == SelectTarget::Clips);
    // Both live: the range wins here only because the view never lets both
    // live (a clip gesture clears the range, a range clears the clips) — the
    // order is pinned so a future caller cannot make it mean the other thing.
    CHECK(SelectionTargetFor(true,  true)  == SelectTarget::Range);

    // The invariant the view enforces, spelled out as the user's gestures:
    // click a clip (clips selected, no range) -> Delete acts on the clips;
    // drag a range (range set, clips dropped) -> Delete acts on the range.
    bool range = false, clips = false;
    clips = true;  range = false;                       // click a clip
    CHECK(SelectionTargetFor(range, clips) == SelectTarget::Clips);
    range = true;  clips = false;                       // drag a range
    CHECK(SelectionTargetFor(range, clips) == SelectTarget::Range);
}

// --- the marker move, through the command stack -----------------------------
static void test_move_marker() {
    std::printf("move marker\n");
    Project p;
    CommandStack s;
    p.markers.push_back(Marker{1000, "Verse"});
    p.markers.push_back(Marker{5000, "Chorus"});
    p.markers.push_back(Marker{9000, "Bridge"});

    // A plain move inside its own slot: one undo step, right label.
    CHECK(s.Execute(std::make_unique<MoveMarkerCommand>(5000, "Chorus", 6000), p));
    CHECK(p.markers.size() == 3);
    CHECK(p.markers[1].frame == 6000 && p.markers[1].name == "Chorus");
    CHECK(s.UndoName() == "Move Marker");
    s.Undo(p);
    CHECK(p.markers[1].frame == 5000 && p.markers[1].name == "Chorus");

    // A move ACROSS another marker reorders the vector (every reader assumes
    // it is sorted by frame).
    CHECK(s.Execute(std::make_unique<MoveMarkerCommand>(1000, "Verse", 7000), p));
    CHECK(p.markers.size() == 3);
    CHECK(p.markers[0].frame == 5000);    // Chorus
    CHECK(p.markers[1].frame == 7000);    // Verse, moved
    CHECK(p.markers[2].frame == 9000);    // Bridge
    s.Undo(p);
    CHECK(p.markers.size() == 3);
    CHECK(p.markers[0].frame == 1000 && p.markers[0].name == "Verse");
    CHECK(p.markers[1].frame == 5000 && p.markers[1].name == "Chorus");
    CHECK(p.markers[2].frame == 9000 && p.markers[2].name == "Bridge");

    // Redo re-applies it (the command is reusable).
    s.Redo(p);
    CHECK(p.markers[1].frame == 7000 && p.markers[1].name == "Verse");

    // Two markers on one frame: the name says which one moves.
    Project q;
    q.markers.push_back(Marker{2000, "A"});
    q.markers.push_back(Marker{2000, "B"});
    CommandStack s2;
    CHECK(s2.Execute(std::make_unique<MoveMarkerCommand>(2000, "B", 3000), q));
    CHECK(q.markers[0].name == "A" && q.markers[0].frame == 2000);
    CHECK(q.markers[1].name == "B" && q.markers[1].frame == 3000);
    s2.Undo(q);
    CHECK(q.markers[1].frame == 2000 && q.markers[1].name == "B");

    // A marker that is not there fails, so nothing is pushed (a failed Do is
    // not an undo step).
    Project r;
    CommandStack s3;
    CHECK(!s3.Execute(std::make_unique<MoveMarkerCommand>(1234, "Nope", 4321), r));
    CHECK(!s3.CanUndo());
}

int main() {
    test_topmost_clip();
    test_default_loop_range();
    test_scroll_steps();
    test_selection_target();
    test_move_marker();
    std::printf("%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
