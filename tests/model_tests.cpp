// Host-buildable unit tests for the model layer. No Haiku kits required —
// this compiles and runs on Linux/macOS/Haiku alike, so model logic gets
// verified without the VM roundtrip.
//
// Tiny hand-rolled assert harness (no gtest dependency to keep the build
// trivial on Haiku).

#include "../src/model/Commands.h"

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
    test_mute_solo_undo_redo();
    test_effect_commands();
    test_frame_seconds_roundtrip();

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
