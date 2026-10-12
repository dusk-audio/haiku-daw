// Host tests for the arrange editing commands added by the arrange-feel
// package (M2.1-2.3): Glue's two joins and the slip edit.
//
//   g++ -std=c++17 -Isrc tests/arrange_cmd_tests.cpp src/model/*.cpp ...

#include "../src/model/Commands.h"
#include "../src/model/Project.h"

#include <cstdio>
#include <memory>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Two touching audio clips as a split leaves them: [0,1000) and [1000,2500),
// the right one's source advanced by 1000 frames.
static void SplitPairTrack(Project& p, ClipId* leftId, ClipId* rightId) {
    const TrackId tid = 7;
    Track t;
    t.id = tid;
    t.type = TrackType::Audio;
    t.name = "A";
    p.AddTrack(t);

    Clip a;
    a.id = p.NextClipId();
    a.startFrame = 0;
    a.lengthFrames = 1000;
    a.fadeOutFrames = 120;          // a seam fade the join must clear
    a.sourcePath = "x.wav";
    p.AddClip(tid, a);

    Clip b = a;
    b.id = p.NextClipId();
    b.startFrame = 1000;
    b.lengthFrames = 1500;
    b.sourceOffset = 1000;
    b.fadeInFrames = 120;
    b.fadeOutFrames = 0;
    p.AddClip(tid, b);

    *leftId = a.id;
    *rightId = b.id;
}

int main() {
    // --- JoinClipsCommand -------------------------------------------------
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        CommandStack st;

        CHECK(st.Execute(std::make_unique<JoinClipsCommand>(7, lid), p));
        const Track* t = p.FindTrack(7);
        CHECK(t != nullptr);
        CHECK(t->clips.size() == 1);
        if (t && !t->clips.empty()) {
            const Clip& c = t->clips[0];
            CHECK(c.id == lid);                       // the left one survives
            CHECK(c.startFrame == 0);
            CHECK(c.lengthFrames == 2500);            // covers the right's end
            CHECK(c.sourceOffset == 0);               // source untouched
            CHECK(c.fadeOutFrames == 0);              // interior seam cleared
        }

        // One undo step restores both halves exactly, ids included.
        CHECK(st.Undo(p));
        t = p.FindTrack(7);
        CHECK(t != nullptr && t->clips.size() == 2);
        if (t && t->clips.size() == 2) {
            CHECK(t->clips[0].id == lid && t->clips[1].id == rid);
            CHECK(t->clips[0].lengthFrames == 1000);
            CHECK(t->clips[0].fadeOutFrames == 120);
            CHECK(t->clips[1].startFrame == 1000);
            CHECK(t->clips[1].lengthFrames == 1500);
            CHECK(t->clips[1].sourceOffset == 1000);
        }
        // Redo re-applies it (the cached right clip must not be added twice).
        CHECK(st.Redo(p));
        CHECK(p.FindTrack(7)->clips.size() == 1);
    }

    // A gap refuses the join: nothing is pushed, the model is untouched.
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        p.FindTrack(7)->clips[1].startFrame = 4000;   // a gap between the halves
        CommandStack st;
        CHECK(!st.Execute(std::make_unique<JoinClipsCommand>(7, lid), p));
        CHECK(!st.CanUndo());
        CHECK(p.FindTrack(7)->clips.size() == 2);
        CHECK(p.FindTrack(7)->clips[0].lengthFrames == 1000);
    }

    // Overlapping clips join too (an overlap is a crossfade, still a seam).
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        p.FindTrack(7)->clips[1].startFrame = 800;   // 200-frame overlap
        CommandStack st;
        CHECK(st.Execute(std::make_unique<JoinClipsCommand>(7, lid), p));
        CHECK(p.FindTrack(7)->clips.size() == 1);
        CHECK(p.FindTrack(7)->clips[0].lengthFrames == 2300);
    }

    // The last clip has nothing to its right: a no-op, not an undo entry.
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        CommandStack st;
        CHECK(!st.Execute(std::make_unique<JoinClipsCommand>(7, rid), p));
        CHECK(!st.CanUndo());
        CHECK(p.FindTrack(7)->clips.size() == 2);
    }

    // Take-group members are never glued together.
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        p.FindTrack(7)->clips[0].takeGroup = 3;
        p.FindTrack(7)->clips[1].takeGroup = 3;
        CommandStack st;
        CHECK(!st.Execute(std::make_unique<JoinClipsCommand>(7, lid), p));
        CHECK(p.FindTrack(7)->clips.size() == 2);
    }

    // --- JoinMidiClipsCommand --------------------------------------------
    {
        Project p;
        Track t;
        t.id = 9;
        t.type = TrackType::Midi;
        p.AddTrack(t);

        MidiClip a;
        a.id = p.NextClipId();
        a.startFrame = 1000;
        a.lengthFrames = 2000;
        a.notes.push_back({ 60, 100, 500, 200 });    // 500 into the left region
        a.fadeOutFrames = 300;
        p.AddMidiClip(9, a);

        MidiClip b;
        b.id = p.NextClipId();
        b.startFrame = 3000;
        b.lengthFrames = 1000;
        b.notes.push_back({ 64, 90, 100, 200 });     // 100 into the right region
        b.events.push_back({ MidiClipEvent::CC, 50, 7, 100 });
        p.AddMidiClip(9, b);

        CommandStack st;
        CHECK(st.Execute(std::make_unique<JoinMidiClipsCommand>(9, a.id), p));
        Track* jt = p.FindTrack(9);
        CHECK(jt != nullptr && jt->midiClips.size() == 1);
        if (jt && !jt->midiClips.empty()) {
            const MidiClip& c = jt->midiClips[0];
            CHECK(c.id == a.id);
            CHECK(c.startFrame == 1000);
            CHECK(c.lengthFrames == 3000);           // to the right region's end
            CHECK(c.fadeOutFrames == 0);
            CHECK(c.notes.size() == 2);
            // The right region's note keeps its place in time: 3000 + 100 is
            // 2100 frames after the joined region's start.
            if (c.notes.size() == 2) {
                CHECK(c.notes[0].startFrame == 500);
                CHECK(c.notes[1].startFrame == 2100);
                CHECK(c.notes[1].pitch == 64);
            }
            CHECK(c.events.size() == 1);
            if (c.events.size() == 1)
                CHECK(c.events[0].startFrame == 2050);
        }

        CHECK(st.Undo(p));
        jt = p.FindTrack(9);
        CHECK(jt != nullptr && jt->midiClips.size() == 2);
        if (jt && jt->midiClips.size() == 2) {
            CHECK(jt->midiClips[0].id == a.id);
            CHECK(jt->midiClips[0].lengthFrames == 2000);
            CHECK(jt->midiClips[0].notes.size() == 1);
            CHECK(jt->midiClips[0].fadeOutFrames == 300);
            CHECK(jt->midiClips[1].id == b.id);
            CHECK(jt->midiClips[1].notes.size() == 1);
            CHECK(jt->midiClips[1].notes[0].startFrame == 100);
            CHECK(jt->midiClips[1].events.size() == 1);
        }
    }

    // --- SlipClipCommand --------------------------------------------------
    {
        Project p;
        ClipId lid = 0, rid = 0;
        SplitPairTrack(p, &lid, &rid);
        CommandStack st;
        CHECK(st.Execute(std::make_unique<SlipClipCommand>(7, lid, 480), p));
        const Clip* c = p.FindTrack(7)->FindClip(lid);
        CHECK(c != nullptr);
        if (c) {
            CHECK(c->sourceOffset == 480);
            CHECK(c->startFrame == 0);            // nothing else moved
            CHECK(c->lengthFrames == 1000);
            CHECK(c->fadeOutFrames == 120);
        }
        CHECK(st.Undo(p));
        CHECK(p.FindTrack(7)->FindClip(lid)->sourceOffset == 0);

        // A negative offset clamps at 0 (there is no audio before the file).
        CHECK(st.Execute(std::make_unique<SlipClipCommand>(7, lid, -900), p));
        CHECK(p.FindTrack(7)->FindClip(lid)->sourceOffset == 0);

        // A missing clip is a no-op, not an undo entry.
        CommandStack st2;
        CHECK(!st2.Execute(std::make_unique<SlipClipCommand>(7, 99999, 100), p));
        CHECK(!st2.CanUndo());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
