// Host-buildable round-trip test for ProjectIO: build a project with an audio
// track (clip + fx) and a MIDI track (notes), save it, load it into a fresh
// Project, and check everything survived. Also checks id allocators are bumped
// so a post-load edit doesn't reuse a loaded id.

#include "../src/model/ProjectIO.h"
#include "../src/model/Commands.h"

#include <cstdio>
#include <unistd.h>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const char* path = "projectio_test_tmp.dawproj";

    Project a;
    CommandStack stack;
    a.sampleRate = 44100.0;
    a.tempoBPM   = 90.0;
    a.transport.playhead = 12345;

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr L"), a);
    TrackId t1 = a.Tracks().front().id;
    stack.Execute(std::make_unique<SetTrackGainCommand>(t1, 0.5f), a);
    stack.Execute(std::make_unique<SetTrackPanCommand>(t1, -0.3f), a);
    Clip c; c.startFrame = 1000; c.lengthFrames = 2000; c.sourceOffset = 10;
    c.sourcePath = "takes/one two.wav";   // note the space
    stack.Execute(std::make_unique<AddClipCommand>(t1, c), a);
    stack.Execute(std::make_unique<AddEffectCommand>(t1, LowPassDesc(700.0f)), a);

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Synth"), a);
    TrackId t2 = a.Tracks().back().id;
    MidiNote n; n.pitch = 64; n.velocity = 90; n.startFrame = 500; n.lengthFrames = 480;
    stack.Execute(std::make_unique<AddNoteCommand>(t2, n), a);

    CHECK(ProjectIO::Save(a, path));

    Project b;
    CHECK(ProjectIO::Load(b, path));
    CHECK(b.sampleRate == 44100.0);
    CHECK(b.tempoBPM == 90.0);
    CHECK(b.transport.playhead == 12345);
    CHECK(b.Tracks().size() == 2);

    const Track& bt1 = b.Tracks()[0];
    CHECK(bt1.id == t1);
    CHECK(bt1.name == "Gtr L");
    CHECK(bt1.type == TrackType::Audio);
    CHECK(std::abs(bt1.gain - 0.5f) < 1e-4f);
    CHECK(std::abs(bt1.pan - (-0.3f)) < 1e-4f);
    CHECK(bt1.clips.size() == 1);
    CHECK(bt1.clips[0].startFrame == 1000);
    CHECK(bt1.clips[0].lengthFrames == 2000);
    CHECK(bt1.clips[0].sourcePath == "takes/one two.wav");
    CHECK(bt1.fx.size() == 1);
    CHECK(bt1.fx[0].type == EffectType::Biquad);

    const Track& bt2 = b.Tracks()[1];
    CHECK(bt2.type == TrackType::Midi);
    CHECK(bt2.name == "Synth");
    CHECK(bt2.notes.size() == 1);
    CHECK(bt2.notes[0].pitch == 64);
    CHECK(bt2.notes[0].startFrame == 500);

    // A new track after load must get a fresh id, not collide with loaded ones.
    CommandStack s2;
    s2.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "New"), b);
    TrackId newId = b.Tracks().back().id;
    CHECK(newId != t1 && newId != t2);

    std::remove(path);

    // Relative media paths: a clip whose absolute path is under the project
    // file's directory is stored relative and resolves back on load, so a
    // project + its media are portable together.
    {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd))) {
            const std::string base = cwd;
            const std::string projPath = base + "/proj_reltest.dawproj";
            const std::string mediaAbs = base + "/media/loop.wav";

            Project r;
            CommandStack rs;
            rs.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "T"), r);
            TrackId rt = r.Tracks().front().id;
            Clip rc; rc.startFrame = 0; rc.lengthFrames = 1;
            rc.sourcePath = mediaAbs;
            rs.Execute(std::make_unique<AddClipCommand>(rt, rc), r);

            CHECK(ProjectIO::Save(r, projPath));
            Project r2;
            CHECK(ProjectIO::Load(r2, projPath));
            // Round-trips back to the same absolute path.
            CHECK(r2.Tracks()[0].clips[0].sourcePath == mediaAbs);
            std::remove(projPath.c_str());
        }
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
