// Host tests for the track-depth commands the arrange-tracks package adds
// (M2.6): DuplicateTrackCommand and SetTrackCollapsedCommand.
//
//   g++ -std=c++17 -Isrc tests/track_depth_tests.cpp src/model/*.cpp ...

#include "../src/model/Commands.h"
#include "../src/model/Project.h"
#include "../src/model/ProjectIO.h"

#include <cstdio>
#include <memory>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// A track that carries one of everything a duplicate must copy: clips, a
// region with a note, an effect, a send, an input and an automation point.
static Track MakeBusyTrack(Project& p) {
    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "drums";
    t.gain = 0.8f;
    t.pan = -0.25f;
    t.muted = true;
    t.soloSafe = true;
    t.colorIndex = 3;
    t.height = 96;
    t.collapsed = true;
    t.input = InputSource{ InputSource::kAudioDefault, "", 0 };
    t.gainAuto.AddPoint(1000, 0.5f);
    Send s; s.dest = 7; s.level = 0.4f; s.preFader = true;
    t.sends.push_back(s);
    EffectDesc fx; fx.type = EffectType::Eq;
    t.fx.push_back(fx);

    Clip c;
    c.id = p.NextClipId();
    c.startFrame = 100;
    c.lengthFrames = 500;
    c.sourcePath = "a.wav";
    c.name = "verse";
    t.clips.push_back(c);

    Clip c2 = c;
    c2.id = p.NextClipId();
    c2.startFrame = 700;
    t.clips.push_back(c2);

    MidiClip m;
    m.id = p.NextClipId();
    m.startFrame = 0;
    m.lengthFrames = 400;
    m.notes.push_back({ 60, 100, 0, 100 });
    t.midiClips.push_back(m);

    // Freeze stash: its clips become live again on Unfreeze, so a copy's stash
    // needs fresh ids too.
    Clip f = c;
    f.id = p.NextClipId();
    t.freezeClips.push_back(f);
    return t;
}

int main() {
    // --- SetTrackCollapsedCommand -----------------------------------------
    {
        Project p;
        CommandStack st;
        Track t;
        t.id = p.NextTrackId();
        t.name = "A";
        p.AddTrack(t);
        CHECK(p.FindTrack(t.id)->collapsed == false);   // the default

        CHECK(st.Execute(std::make_unique<SetTrackCollapsedCommand>(t.id, true), p));
        CHECK(p.FindTrack(t.id)->collapsed == true);
        CHECK(st.UndoName() == "Collapse Track");
        CHECK(st.Undo(p));
        CHECK(p.FindTrack(t.id)->collapsed == false);
        CHECK(st.Redo(p));
        CHECK(p.FindTrack(t.id)->collapsed == true);
        // A track that is gone refuses rather than crashing.
        CHECK(!SetTrackCollapsedCommand(9999, true).Do(p));
    }

    // --- DuplicateTrackCommand --------------------------------------------
    {
        Project p;
        CommandStack st;
        const Track busy = MakeBusyTrack(p);
        p.AddTrack(busy);
        Track other;
        other.id = p.NextTrackId();
        other.name = "keys";
        p.AddTrack(other);
        CHECK(p.Tracks().size() == 2);

        auto dup = std::make_unique<DuplicateTrackCommand>(busy.id);
        DuplicateTrackCommand* dp = dup.get();
        CHECK(st.Execute(std::move(dup), p));
        CHECK(st.UndoName() == "Duplicate Track");
        CHECK(p.Tracks().size() == 3);

        const TrackId newId = dp->CreatedId();
        CHECK(newId != busy.id);
        CHECK(dp->NewIndex() == 1);                   // right after the source
        CHECK(p.Tracks()[1].id == newId);

        const Track* d = p.FindTrack(newId);
        CHECK(d != nullptr);
        if (d) {
            // The copy carries everything, with the copy suffix.
            CHECK(d->name == "drums copy");
            CHECK(d->type == busy.type);
            CHECK(d->gain == busy.gain && d->pan == busy.pan);
            CHECK(d->muted == busy.muted && d->soloSafe == busy.soloSafe);
            CHECK(d->colorIndex == busy.colorIndex);
            CHECK(d->height == busy.height);
            CHECK(d->collapsed == busy.collapsed);
            CHECK(d->input.kind == busy.input.kind);
            CHECK(d->fx.size() == 1 && d->sends.size() == 1);
            CHECK(d->gainAuto.Count() == 1);
            // Same clip geometry and content...
            CHECK(d->clips.size() == 2);
            if (d->clips.size() == 2) {
                CHECK(d->clips[0].startFrame == 100);
                CHECK(d->clips[0].lengthFrames == 500);
                CHECK(d->clips[0].sourcePath == "a.wav");
                CHECK(d->clips[0].name == "verse");
            }
            CHECK(d->midiClips.size() == 1);
            if (!d->midiClips.empty())
                CHECK(d->midiClips[0].notes.size() == 1);
            CHECK(d->freezeClips.size() == 1);
            // ... but NO shared clip id: an id-addressed edit (a move, a
            // rename) must never reach two blocks at once.
            for (const Clip& c : busy.clips)
                for (const Clip& dc : d->clips)
                    CHECK(c.id != dc.id);
            if (!d->midiClips.empty() && !busy.midiClips.empty())
                CHECK(d->midiClips[0].id != busy.midiClips[0].id);
            if (!d->freezeClips.empty() && !busy.freezeClips.empty())
                CHECK(d->freezeClips[0].id != busy.freezeClips[0].id);
        }

        // The ids survive an undo/redo pair: redo must not re-allocate them,
        // or every id-addressed selection would be left pointing at nothing.
        const ClipId firstClipId = d ? d->clips[0].id : 0;
        const ClipId firstRegionId = d && !d->midiClips.empty()
                                     ? d->midiClips[0].id : 0;
        CHECK(st.Undo(p));
        CHECK(p.Tracks().size() == 2);
        CHECK(p.FindTrack(newId) == nullptr);
        CHECK(st.Redo(p));
        const Track* again = p.FindTrack(newId);
        CHECK(again != nullptr);
        if (again) {
            CHECK(again->name == "drums copy");
            CHECK(again->clips.size() == 2);
            CHECK(again->clips[0].id == firstClipId);
            CHECK(again->midiClips.size() == 1);
            if (!again->midiClips.empty())
                CHECK(again->midiClips[0].id == firstRegionId);
        }

        // A second duplicate allocates its own track id and leaves the first
        // copy alone.
        CHECK(st.Execute(std::make_unique<DuplicateTrackCommand>(busy.id), p));
        CHECK(p.Tracks().size() == 4);
        CHECK(p.FindTrack(newId) != nullptr);

        // Duplicating a track that is gone refuses.
        CHECK(!DuplicateTrackCommand(9999).Do(p));
    }

    // --- Duplicate + ProjectIO round trip ---------------------------------
    {
        Project p;
        Track t;
        t.id = p.NextTrackId();
        t.name = "IO";
        t.collapsed = true;
        p.AddTrack(t);
        const std::string path = "/tmp/haiku_daw_trackdepth_io.dawproj";
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        CHECK(q.Tracks().size() == 1);
        if (q.Tracks().size() == 1) {
            CHECK(q.Tracks()[0].name == "IO");
            CHECK(q.Tracks()[0].collapsed == true);
        }
        std::remove(path.c_str());
    }

    std::printf("track_depth_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
