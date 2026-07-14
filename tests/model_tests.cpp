// Host-buildable unit tests for the model layer. No Haiku kits required —
// this compiles and runs on Linux/macOS/Haiku alike, so model logic gets
// verified without the VM roundtrip.
//
// Tiny hand-rolled assert harness (no gtest dependency to keep the build
// trivial on Haiku).

#include "../src/model/Commands.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

static void test_add_and_undo_track() {
    std::printf("test_add_and_undo_track\n");
    Project p;
    CommandStack stack;

    auto cmd = std::make_unique<AddTrackCommand>(TrackType::Audio, "Drums");
    AddTrackCommand* raw = cmd.get();
    CHECK(stack.Execute(std::move(cmd), p));
    CHECK(p.Tracks().size() == 1);
    TrackId id = raw->CreatedId();
    CHECK(p.FindTrack(id) != nullptr);
    CHECK(p.FindTrack(id)->name == "Drums");

    CHECK(stack.CanUndo());
    CHECK(stack.Undo(p));
    CHECK(p.Tracks().empty());

    CHECK(stack.CanRedo());
    CHECK(stack.Redo(p));
    CHECK(p.Tracks().size() == 1);
    // Redo must restore the SAME id, not allocate a new one.
    CHECK(p.FindTrack(id) != nullptr);
}

static void test_gain_undo_redo() {
    std::printf("test_gain_undo_redo\n");
    Project p;
    CommandStack stack;

    auto add = std::make_unique<AddTrackCommand>(TrackType::Audio, "Bass");
    TrackId id = add->CreatedId();     // still invalid until Do()
    stack.Execute(std::move(add), p);
    id = p.Tracks().front().id;
    CHECK(p.FindTrack(id)->gain == 1.0f);

    stack.Execute(std::make_unique<SetTrackGainCommand>(id, 0.5f), p);
    CHECK(p.FindTrack(id)->gain == 0.5f);

    stack.Undo(p);
    CHECK(p.FindTrack(id)->gain == 1.0f);
    stack.Redo(p);
    CHECK(p.FindTrack(id)->gain == 0.5f);
}

static void test_clip_sorted_insert_and_move() {
    std::printf("test_clip_sorted_insert_and_move\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr"), p);
    TrackId tid = p.Tracks().front().id;

    // Insert out of order; expect them sorted by startFrame.
    Clip a; a.startFrame = 48000; a.lengthFrames = 1000; a.sourcePath = "a.wav";
    Clip b; b.startFrame = 0;     b.lengthFrames = 1000; b.sourcePath = "b.wav";
    Clip c; c.startFrame = 24000; c.lengthFrames = 1000; c.sourcePath = "c.wav";

    auto ca = std::make_unique<AddClipCommand>(tid, a);
    auto cb = std::make_unique<AddClipCommand>(tid, b);
    auto cc = std::make_unique<AddClipCommand>(tid, c);
    stack.Execute(std::move(ca), p);
    stack.Execute(std::move(cb), p);
    ClipId cId = cc->CreatedId();      // invalid until executed
    stack.Execute(std::move(cc), p);

    const Track* t = p.FindTrack(tid);
    CHECK(t->clips.size() == 3);
    CHECK(t->clips[0].startFrame == 0);
    CHECK(t->clips[1].startFrame == 24000);
    CHECK(t->clips[2].startFrame == 48000);

    // Grab the id of the clip currently at start 0 (was b) and move it late.
    ClipId bId = t->clips[0].id;
    stack.Execute(std::make_unique<MoveClipCommand>(tid, bId, 96000), p);
    t = p.FindTrack(tid);
    CHECK(t->clips.back().id == bId);
    CHECK(t->clips.back().startFrame == 96000);
    CHECK(t->clips.front().startFrame == 24000);   // c is now earliest

    // Undo the move -> b returns to the front at 0.
    stack.Undo(p);
    t = p.FindTrack(tid);
    CHECK(t->clips.front().id == bId);
    CHECK(t->clips.front().startFrame == 0);

    (void)cId;
}

static void test_split_clip() {
    std::printf("test_split_clip\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr"), p);
    TrackId tid = p.Tracks().front().id;

    Clip a; a.startFrame = 1000; a.lengthFrames = 4000; a.sourceOffset = 500;
    a.sourcePath = "a.wav"; a.gain = 0.8f; a.fadeOutFrames = 300;
    auto ac = std::make_unique<AddClipCommand>(tid, a);
    ClipId aId = ac->CreatedId();
    stack.Execute(std::move(ac), p);
    aId = p.FindTrack(tid)->clips.front().id;

    // Split at frame 3000 (2000 into the 4000-long clip).
    stack.Execute(std::make_unique<SplitClipCommand>(tid, aId, 3000), p);
    const Track* t = p.FindTrack(tid);
    CHECK(t->clips.size() == 2);
    const Clip& L = t->clips[0];
    const Clip& R = t->clips[1];
    CHECK(L.startFrame == 1000 && L.lengthFrames == 2000);
    CHECK(R.startFrame == 3000 && R.lengthFrames == 2000);
    CHECK(R.sourceOffset == 500 + 2000);      // offset advanced by the left length
    CHECK(L.fadeOutFrames == 0);              // interior cut clears left fade-out
    CHECK(R.fadeOutFrames == 300);            // right keeps the original fade-out
    CHECK(std::fabs(R.gain - 0.8f) < 1e-4f);  // gain copied

    // Split outside the clip is a no-op.
    CHECK(!SplitClipCommand(tid, aId, 500).Do(p));

    // Undo restores the single clip.
    stack.Undo(p);
    t = p.FindTrack(tid);
    CHECK(t->clips.size() == 1);
    CHECK(t->clips[0].lengthFrames == 4000);
    CHECK(t->clips[0].fadeOutFrames == 300);
}

static void test_active_take() {
    std::printf("test_active_take\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "T"), p);
    TrackId tid = p.Tracks().front().id;
    // Three stacked takes (group 1) at the same position; the first is active.
    ClipId ids[3];
    for (int i = 0; i < 3; i++) {
        Clip c; c.id = p.NextClipId(); ids[i] = c.id;
        c.startFrame = 0; c.lengthFrames = 1000; c.sourcePath = "t.wav";
        c.takeGroup = 1; c.takeActive = (i == 0); c.sourceOffset = i * 1000;
        p.AddClip(tid, c);
    }
    auto activeId = [&]() {
        const Track* tr = p.FindTrack(tid);
        int n = 0; ClipId a = kInvalidClipId;
        for (const Clip& c : tr->clips) if (c.takeActive) { n++; a = c.id; }
        return std::make_pair(n, a);
    };
    CHECK(activeId() == std::make_pair(1, ids[0]));

    // Select take ids[1] -> only it active.
    stack.Execute(std::make_unique<SetActiveTakeCommand>(tid, ids[1]), p);
    CHECK(activeId() == std::make_pair(1, ids[1]));

    // Undo restores ids[0] active.
    stack.Undo(p);
    CHECK(activeId() == std::make_pair(1, ids[0]));
}

static void test_undo_unification() {
    std::printf("test_undo_unification\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "S"), p);
    TrackId tid = p.Tracks().front().id;

    // SetFxCommand (track).
    stack.Execute(std::make_unique<SetFxCommand>(
        tid, false, std::vector<EffectDesc>{ ReverbDesc(), DelayDesc() }), p);
    CHECK(p.FindTrack(tid)->fx.size() == 2);
    stack.Undo(p);
    CHECK(p.FindTrack(tid)->fx.empty());
    stack.Redo(p);
    CHECK(p.FindTrack(tid)->fx.size() == 2);

    // SetFxCommand (master).
    stack.Execute(std::make_unique<SetFxCommand>(
        kInvalidTrackId, true, std::vector<EffectDesc>{ EqDesc() }), p);
    CHECK(p.masterFx.size() == 1);
    stack.Undo(p);
    CHECK(p.masterFx.empty());

    // SetNotesCommand.
    MidiNote n; n.pitch = 60; n.startFrame = 0; n.lengthFrames = 100;
    stack.Execute(std::make_unique<SetNotesCommand>(
        tid, std::vector<MidiNote>{ n }), p);
    CHECK(p.FindTrack(tid)->notes.size() == 1);
    stack.Undo(p);
    CHECK(p.FindTrack(tid)->notes.empty());

    // Coalescing: two consecutive SetInstrumentCommand -> ONE undo step that
    // restores the original instrument (before the first).
    const int origWave = p.FindTrack(tid)->instrument.waveform;
    Instrument i1; i1.waveform = 1;
    Instrument i2; i2.waveform = 3;
    stack.Execute(std::make_unique<SetInstrumentCommand>(tid, i1), p);
    stack.Execute(std::make_unique<SetInstrumentCommand>(tid, i2), p);
    CHECK(p.FindTrack(tid)->instrument.waveform == 3);   // latest applied
    CHECK(stack.Undo(p));                                 // single undo...
    CHECK(p.FindTrack(tid)->instrument.waveform == origWave);  // ...to original
    // Only one entry existed for the coalesced pair (next undo hits the note cmd
    // era, not a second instrument step) — verify the instrument didn't step.
    CHECK(p.FindTrack(tid)->instrument.waveform == origWave);

    // SetTrackColor / Height do + undo.
    stack.Execute(std::make_unique<SetTrackColorCommand>(tid, 3), p);
    CHECK(p.FindTrack(tid)->colorIndex == 3);
    stack.Undo(p);
    CHECK(p.FindTrack(tid)->colorIndex == 0);
}

static void test_macro_command() {
    std::printf("test_macro_command\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "T"), p);
    TrackId tid = p.Tracks().front().id;
    Clip a; a.startFrame = 0;    a.lengthFrames = 100; a.sourcePath = "a.wav";
    Clip b; b.startFrame = 200;  b.lengthFrames = 100; b.sourcePath = "b.wav";
    stack.Execute(std::make_unique<AddClipCommand>(tid, a), p);
    stack.Execute(std::make_unique<AddClipCommand>(tid, b), p);
    ClipId aId = p.Tracks()[0].clips[0].id;
    ClipId bId = p.Tracks()[0].clips[1].id;
    CHECK(p.Tracks()[0].clips.size() == 2);

    // Macro delete of both clips = one undo step.
    auto macro = std::make_unique<MacroCommand>("Delete 2");
    macro->Add(std::make_unique<RemoveClipCommand>(tid, aId));
    macro->Add(std::make_unique<RemoveClipCommand>(tid, bId));
    stack.Execute(std::move(macro), p);
    CHECK(p.Tracks()[0].clips.empty());

    stack.Undo(p);                         // single undo restores both
    CHECK(p.Tracks()[0].clips.size() == 2);

    stack.Redo(p);                         // single redo removes both again
    CHECK(p.Tracks()[0].clips.empty());

    // Regression: a macro with a FAILING sub-command must not undo that
    // sub-command (it captured no rollback state). Here a MoveClipToTrack of a
    // clip that isn't on the source track fails; a following AddClip succeeds.
    // Undo must not delete an unrelated clip.
    stack.Undo(p);                          // clips back (2)
    Clip keep; keep.startFrame = 5000; keep.lengthFrames = 100;
    keep.sourcePath = "k.wav";
    stack.Execute(std::make_unique<AddClipCommand>(tid, keep), p);
    const size_t before = p.Tracks()[0].clips.size();   // 3
    ClipId keepId = p.Tracks()[0].clips.back().id;
    auto m2 = std::make_unique<MacroCommand>("mixed");
    m2->Add(std::make_unique<MoveClipToTrackCommand>(tid, 999999 /*absent*/,
                                                     tid, 1000));   // fails
    m2->Add(std::make_unique<SetTrackNameCommand>(tid, "renamed"));  // succeeds
    stack.Execute(std::move(m2), p);
    stack.Undo(p);
    // The failing move's Undo must NOT have run -> keep clip still present.
    CHECK(p.Tracks()[0].clips.size() == before);
    CHECK(p.FindTrack(tid)->FindClip(keepId) != nullptr);
}

static void test_move_track() {
    std::printf("test_move_track\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "A"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "B"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "C"), p);
    TrackId a = p.Tracks()[0].id, b = p.Tracks()[1].id, c = p.Tracks()[2].id;

    // Move B down -> A C B.
    stack.Execute(std::make_unique<MoveTrackCommand>(b, +1), p);
    CHECK(p.Tracks()[0].id == a);
    CHECK(p.Tracks()[1].id == c);
    CHECK(p.Tracks()[2].id == b);

    // Undo -> A B C.
    stack.Undo(p);
    CHECK(p.Tracks()[0].id == a);
    CHECK(p.Tracks()[1].id == b);
    CHECK(p.Tracks()[2].id == c);

    // Move at the edge fails (no undo entry).
    CHECK(!MoveTrackCommand(a, -1).Do(p));
    CHECK(!MoveTrackCommand(c, +1).Do(p));
}

static void test_bus_routing() {
    std::printf("test_bus_routing\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Bus, "Drum Bus"), p);
    TrackId gtr = p.Tracks()[0].id, bus = p.Tracks()[1].id;
    CHECK(p.Tracks()[1].type == TrackType::Bus);
    CHECK(p.FindTrack(gtr)->output == kInvalidTrackId);   // master by default

    stack.Execute(std::make_unique<SetTrackOutputCommand>(gtr, bus), p);
    CHECK(p.FindTrack(gtr)->output == bus);
    stack.Undo(p);
    CHECK(p.FindTrack(gtr)->output == kInvalidTrackId);
    stack.Redo(p);
    CHECK(p.FindTrack(gtr)->output == bus);

    // Self-route is rejected.
    CHECK(!stack.Execute(std::make_unique<SetTrackOutputCommand>(gtr, gtr), p));
}

static void test_move_clip_to_track() {
    std::printf("test_move_clip_to_track\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "A"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "B"), p);
    TrackId ta = p.Tracks()[0].id, tb = p.Tracks()[1].id;
    Clip c; c.startFrame = 100; c.lengthFrames = 50; c.sourcePath = "x.wav";
    auto add = std::make_unique<AddClipCommand>(ta, c);
    AddClipCommand* ap = add.get();
    stack.Execute(std::move(add), p);
    ClipId cid = ap->CreatedId();

    stack.Execute(std::make_unique<MoveClipToTrackCommand>(ta, cid, tb, 400), p);
    CHECK(p.FindTrack(ta)->clips.empty());
    CHECK(p.FindTrack(tb)->clips.size() == 1);
    CHECK(p.FindTrack(tb)->clips[0].id == cid);
    CHECK(p.FindTrack(tb)->clips[0].startFrame == 400);

    stack.Undo(p);   // back to track A at its old start
    CHECK(p.FindTrack(tb)->clips.empty());
    CHECK(p.FindTrack(ta)->clips.size() == 1);
    CHECK(p.FindTrack(ta)->clips[0].startFrame == 100);
}

static void test_resize_clip_and_edit_note() {
    std::printf("test_resize_clip_and_edit_note\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "A"), p);
    TrackId ta = p.Tracks().front().id;
    Clip c; c.startFrame = 0; c.lengthFrames = 1000; c.sourcePath = "x.wav";
    auto add = std::make_unique<AddClipCommand>(ta, c);
    AddClipCommand* ap = add.get();
    stack.Execute(std::move(add), p);
    ClipId cid = ap->CreatedId();

    stack.Execute(std::make_unique<ResizeClipCommand>(ta, cid, 500), p);
    CHECK(p.FindTrack(ta)->clips[0].lengthFrames == 500);
    stack.Undo(p);
    CHECK(p.FindTrack(ta)->clips[0].lengthFrames == 1000);
    // Clamp: length below 1 becomes 1.
    stack.Execute(std::make_unique<ResizeClipCommand>(ta, cid, -5), p);
    CHECK(p.FindTrack(ta)->clips[0].lengthFrames == 1);

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "M"), p);
    TrackId tm = p.Tracks().back().id;
    MidiNote n; n.pitch = 60; n.startFrame = 0; n.lengthFrames = 100;
    stack.Execute(std::make_unique<AddNoteCommand>(tm, n), p);
    MidiNote e; e.pitch = 67; e.velocity = 40; e.startFrame = 480; e.lengthFrames = 240;
    stack.Execute(std::make_unique<NoteEditCommand>(tm, 0, e), p);
    CHECK(p.FindTrack(tm)->notes[0].pitch == 67);
    CHECK(p.FindTrack(tm)->notes[0].velocity == 40);
    CHECK(p.FindTrack(tm)->notes[0].startFrame == 480);
    CHECK(p.FindTrack(tm)->notes[0].lengthFrames == 240);
    stack.Undo(p);
    CHECK(p.FindTrack(tm)->notes[0].pitch == 60);
    CHECK(p.FindTrack(tm)->notes[0].velocity == 100);
    CHECK(p.FindTrack(tm)->notes[0].startFrame == 0);
    CHECK(p.FindTrack(tm)->notes[0].lengthFrames == 100);
}

static void test_track_manage_and_fade() {
    std::printf("test_track_manage_and_fade\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "One"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Two"), p);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Three"), p);
    TrackId t2 = p.Tracks()[1].id;
    CHECK(p.Tracks().size() == 3);

    // Rename middle track.
    stack.Execute(std::make_unique<SetTrackNameCommand>(t2, "Bass"), p);
    CHECK(p.FindTrack(t2)->name == "Bass");
    stack.Undo(p);
    CHECK(p.FindTrack(t2)->name == "Two");
    stack.Redo(p);
    CHECK(p.FindTrack(t2)->name == "Bass");

    // Remove middle track; undo restores it in the same position.
    stack.Execute(std::make_unique<RemoveTrackCommand>(t2), p);
    CHECK(p.Tracks().size() == 2);
    CHECK(p.FindTrack(t2) == nullptr);
    stack.Undo(p);
    CHECK(p.Tracks().size() == 3);
    CHECK(p.Tracks()[1].id == t2);           // back in the middle
    CHECK(p.FindTrack(t2)->name == "Bass");  // with its edited name

    // Clip fades.
    TrackId t1 = p.Tracks().front().id;
    Clip c; c.startFrame = 0; c.lengthFrames = 1000; c.sourcePath = "x.wav";
    auto add = std::make_unique<AddClipCommand>(t1, c);
    AddClipCommand* ap = add.get();
    stack.Execute(std::move(add), p);
    ClipId cid = ap->CreatedId();
    stack.Execute(std::make_unique<SetClipFadeCommand>(t1, cid, 100, 200), p);
    CHECK(p.FindTrack(t1)->clips[0].fadeInFrames == 100);
    CHECK(p.FindTrack(t1)->clips[0].fadeOutFrames == 200);
    stack.Undo(p);
    CHECK(p.FindTrack(t1)->clips[0].fadeInFrames == 0);
    CHECK(p.FindTrack(t1)->clips[0].fadeOutFrames == 0);
}

static void test_mute_solo_undo_redo() {
    std::printf("test_mute_solo_undo_redo\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Vox"), p);
    TrackId id = p.Tracks().front().id;
    CHECK(!p.FindTrack(id)->muted);
    CHECK(!p.FindTrack(id)->soloed);

    stack.Execute(std::make_unique<SetTrackMuteCommand>(id, true), p);
    CHECK(p.FindTrack(id)->muted);
    stack.Undo(p);
    CHECK(!p.FindTrack(id)->muted);
    stack.Redo(p);
    CHECK(p.FindTrack(id)->muted);

    stack.Execute(std::make_unique<SetTrackSoloCommand>(id, true), p);
    CHECK(p.FindTrack(id)->soloed);
    stack.Undo(p);
    CHECK(!p.FindTrack(id)->soloed);
}

static void test_remove_clip_and_note() {
    std::printf("test_remove_clip_and_note\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "A"), p);
    TrackId ta = p.Tracks().front().id;
    Clip c; c.startFrame = 100; c.lengthFrames = 50; c.sourcePath = "x.wav";
    auto add = std::make_unique<AddClipCommand>(ta, c);
    AddClipCommand* addPtr = add.get();
    stack.Execute(std::move(add), p);
    ClipId cid = addPtr->CreatedId();
    CHECK(p.FindTrack(ta)->clips.size() == 1);

    stack.Execute(std::make_unique<RemoveClipCommand>(ta, cid), p);
    CHECK(p.FindTrack(ta)->clips.empty());
    stack.Undo(p);   // clip comes back
    CHECK(p.FindTrack(ta)->clips.size() == 1);
    CHECK(p.FindTrack(ta)->clips[0].startFrame == 100);

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "M"), p);
    TrackId tm = p.Tracks().back().id;
    MidiNote n1; n1.pitch = 60; n1.startFrame = 0;
    MidiNote n2; n2.pitch = 64; n2.startFrame = 480;
    stack.Execute(std::make_unique<AddNoteCommand>(tm, n1), p);
    stack.Execute(std::make_unique<AddNoteCommand>(tm, n2), p);
    CHECK(p.FindTrack(tm)->notes.size() == 2);

    stack.Execute(std::make_unique<RemoveNoteCommand>(tm, 0), p);   // remove n1
    CHECK(p.FindTrack(tm)->notes.size() == 1);
    CHECK(p.FindTrack(tm)->notes[0].pitch == 64);
    stack.Undo(p);   // n1 restored at index 0
    CHECK(p.FindTrack(tm)->notes.size() == 2);
    CHECK(p.FindTrack(tm)->notes[0].pitch == 60);
}

static void test_effect_commands() {
    std::printf("test_effect_commands\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Ld"), p);
    TrackId id = p.Tracks().front().id;
    CHECK(p.FindTrack(id)->fx.empty());

    stack.Execute(std::make_unique<AddEffectCommand>(id, LowPassDesc(500.0f)), p);
    stack.Execute(std::make_unique<AddEffectCommand>(id, DelayDesc()), p);
    CHECK(p.FindTrack(id)->fx.size() == 2);
    CHECK(p.FindTrack(id)->fx[0].type == EffectType::Biquad);
    CHECK(p.FindTrack(id)->fx[1].type == EffectType::Delay);

    stack.Undo(p);   // remove the delay
    CHECK(p.FindTrack(id)->fx.size() == 1);

    stack.Execute(std::make_unique<AddEffectCommand>(id, HighPassDesc()), p);
    CHECK(p.FindTrack(id)->fx.size() == 2);

    stack.Execute(std::make_unique<ClearEffectsCommand>(id), p);
    CHECK(p.FindTrack(id)->fx.empty());
    stack.Undo(p);   // restore the cleared chain
    CHECK(p.FindTrack(id)->fx.size() == 2);
}

static void test_note_commands() {
    std::printf("test_note_commands\n");
    Project p;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Syn"), p);
    TrackId id = p.Tracks().front().id;
    CHECK(p.FindTrack(id)->notes.empty());

    MidiNote n; n.pitch = 64; n.velocity = 90; n.startFrame = 1000; n.lengthFrames = 500;
    stack.Execute(std::make_unique<AddNoteCommand>(id, n), p);
    CHECK(p.FindTrack(id)->notes.size() == 1);
    CHECK(p.FindTrack(id)->notes[0].pitch == 64);
    CHECK(p.FindTrack(id)->notes[0].startFrame == 1000);

    stack.Undo(p);
    CHECK(p.FindTrack(id)->notes.empty());
    stack.Redo(p);
    CHECK(p.FindTrack(id)->notes.size() == 1);
}

static void test_frame_seconds_roundtrip() {
    std::printf("test_frame_seconds_roundtrip\n");
    CHECK(SecondsToFrames(1.0, 48000.0) == 48000);
    CHECK(SecondsToFrames(0.5, 44100.0) == 22050);
    CHECK(FramesToSeconds(48000, 48000.0) == 1.0);
}

int main() {
    test_add_and_undo_track();
    test_gain_undo_redo();
    test_clip_sorted_insert_and_move();
    test_move_clip_to_track();
    test_bus_routing();
    test_remove_clip_and_note();
    test_resize_clip_and_edit_note();
    test_track_manage_and_fade();
    test_mute_solo_undo_redo();
    test_effect_commands();
    test_note_commands();
    test_split_clip();
    test_move_track();
    test_macro_command();
    test_undo_unification();
    test_active_take();
    test_frame_seconds_roundtrip();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
