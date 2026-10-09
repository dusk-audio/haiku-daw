// Host-buildable tests for CommandStack's unsaved-changes tracking (plan M0.1).
//
// The design under test: every undo entry carries a serial number that is never
// reused; a command coalesced into the top entry renews that entry's serial;
// MarkSaved() records the current one; and an empty stack has a base serial (0
// at first, then the serial of the last entry dropped when history is trimmed).
//
// The first two tests are the cases a saved-position index gets wrong: a drag
// that continues after a save moves no position, and undoing everything left
// after a trim does not return to the original project while position zero
// claims it does.

#include "../src/model/Commands.h"

#include <cstdio>
#include <memory>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            ++g_fails;                                                  \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

using namespace daw;

static TrackId AddAudioTrack(Project& p, CommandStack& s, const char* name) {
    auto cmd = std::make_unique<AddTrackCommand>(TrackType::Audio, name);
    AddTrackCommand* raw = cmd.get();
    s.Execute(std::move(cmd), p);
    return raw->CreatedId();
}

static std::vector<Send> SendsTo(TrackId dest, float level) {
    std::vector<Send> v(1);
    v[0].dest  = dest;
    v[0].level = level;
    return v;
}

// A fresh stack is the saved state; execute dirties, a save cleans, undo walks
// back to the saved serial, redo (which reuses the entry's serial, because the
// state is identical) dirties again.
static void test_dirty_across_undo_redo() {
    std::printf("test_dirty_across_undo_redo\n");
    Project p;
    CommandStack s;

    CHECK(!s.IsDirty());
    AddAudioTrack(p, s, "Drums");
    CHECK(s.IsDirty());
    s.MarkSaved();
    CHECK(!s.IsDirty());

    AddAudioTrack(p, s, "Bass");
    CHECK(s.IsDirty());
    CHECK(s.Undo(p));
    CHECK(!s.IsDirty());          // back at the saved state
    CHECK(s.Redo(p));
    CHECK(s.IsDirty());           // ...and away again
    CHECK(s.Undo(p));
    CHECK(!s.IsDirty());
}

// Marc's case 1: the user saves mid-drag, the drag continues, and the post
// folds into the SAME undo entry — so the position does not move. An index
// comparison reads clean here; the renewed serial does not.
static void test_coalesced_drag_after_save_is_dirty() {
    std::printf("test_coalesced_drag_after_save_is_dirty\n");
    Project p;
    CommandStack s;
    const TrackId tid = AddAudioTrack(p, s, "Bass");
    const TrackId bus = AddAudioTrack(p, s, "Verb");

    // The drag's first post lands as its own entry.
    CHECK(s.Execute(std::make_unique<SetSendsCommand>(tid, SendsTo(bus, 0.5f)), p));
    s.MarkSaved();
    CHECK(!s.IsDirty());

    // The drag continues: the post coalesces into that entry (its "old" still
    // predates the whole gesture), so the stack's shape is unchanged...
    CHECK(s.Execute(std::make_unique<SetSendsCommand>(tid, SendsTo(bus, 0.9f)), p));
    CHECK(p.FindTrack(tid)->sends.size() == 1);
    CHECK(p.FindTrack(tid)->sends[0].level == 0.9f);
    // ...but the project has changed, and it must read unsaved.
    CHECK(s.IsDirty());

    // One undo still reverses the whole gesture: back to no sends — which is
    // NOT the state the save recorded, so still dirty.
    CHECK(s.Undo(p));
    CHECK(p.FindTrack(tid)->sends.empty());
    CHECK(s.IsDirty());
}

// Marc's case 2: the project was saved at the start, more edits than the cap
// happened (the oldest history is gone), and the user undoes everything left.
// The state is `extra` edits in, not the original — a position of zero would
// say clean; the base serial does not.
static void test_undo_to_empty_after_a_trim_is_not_the_saved_original() {
    std::printf("test_undo_to_empty_after_a_trim_is_not_the_saved_original\n");
    Project p;
    CommandStack s;
    const size_t cap   = CommandStack::kMaxUndoDepth;
    const size_t extra = 44;

    s.MarkSaved();                // "the project as it was opened", empty history
    for (size_t i = 0; i < cap + extra; i++)
        s.Execute(std::make_unique<AddMarkerCommand>((Frame)(i * 1000), "m"), p);
    CHECK(s.IsDirty());

    size_t undone = 0;
    while (s.CanUndo()) { s.Undo(p); undone++; }
    CHECK(undone == cap);
    CHECK(p.markers.size() == extra);   // 44 edits in, not the original
    CHECK(s.IsDirty());                 // NOT clean, despite the empty stack
}

// The converse: saved AT the emptied-trimmed state, an edit away and back is
// clean again — the base serial is a real position, not just "not zero".
static void test_save_at_the_trimmed_empty_state_then_cycle() {
    std::printf("test_save_at_the_trimmed_empty_state_then_cycle\n");
    Project p;
    CommandStack s;
    const size_t cap = CommandStack::kMaxUndoDepth;

    for (size_t i = 0; i < cap + 10; i++)
        s.Execute(std::make_unique<AddMarkerCommand>((Frame)(i * 1000), "m"), p);
    while (s.CanUndo()) s.Undo(p);

    s.MarkSaved();
    CHECK(!s.IsDirty());
    s.Execute(std::make_unique<AddMarkerCommand>(0, "x"), p);
    CHECK(s.IsDirty());
    CHECK(s.Undo(p));
    CHECK(!s.IsDirty());
}

// The saved state's own entry can be trimmed away: it is then reachable only
// from the other side, and the base serial is what proves the state matches.
static void test_saved_entry_trimmed_away_is_still_the_saved_state() {
    std::printf("test_saved_entry_trimmed_away_is_still_the_saved_state\n");
    Project p;
    CommandStack s;
    const size_t cap = CommandStack::kMaxUndoDepth;

    AddAudioTrack(p, s, "A");     // entry 1: the state the user saves
    s.MarkSaved();
    for (size_t i = 0; i < cap; i++)
        s.Execute(std::make_unique<AddMarkerCommand>((Frame)(i * 1000), "m"), p);

    // Exactly one entry past the cap: the saved entry was the one dropped, so
    // the base serial IS its serial.
    size_t undone = 0;
    while (s.CanUndo()) { s.Undo(p); undone++; }
    CHECK(undone == cap);
    CHECK(p.Tracks().size() == 1);      // the saved state, reached from below
    CHECK(p.markers.empty());
    CHECK(!s.IsDirty());
}

// A recovered session is unsaved work: the recovery file is not the project,
// so MarkUnsaved() must read dirty from ANY position (including an empty
// stack), and only a real save or load clears it.
static void test_mark_unsaved() {
    std::printf("test_mark_unsaved\n");
    Project p;
    CommandStack s;
    CHECK(!s.IsDirty());
    s.MarkUnsaved();
    CHECK(s.IsDirty());              // nowhere on disk
    AddAudioTrack(p, s, "A");
    CHECK(s.IsDirty());
    CHECK(s.Undo(p));
    CHECK(s.IsDirty());              // still nowhere, even empty
    s.MarkSaved();                   // the user picks a path and saves
    CHECK(!s.IsDirty());
}

int main() {
    test_dirty_across_undo_redo();
    test_coalesced_drag_after_save_is_dirty();
    test_undo_to_empty_after_a_trim_is_not_the_saved_original();
    test_save_at_the_trimmed_empty_state_then_cycle();
    test_saved_entry_trimmed_away_is_still_the_saved_state();
    test_mark_unsaved();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
