// Host-buildable round-trip test for ProjectIO: build a project with an audio
// track (clip + fx) and a MIDI track (notes), save it, load it into a fresh
// Project, and check everything survived. Also checks id allocators are bumped
// so a post-load edit doesn't reuse a loaded id.

#include "../src/model/ProjectIO.h"
#include "../src/model/Commands.h"

#include <cstdio>
#include <fstream>
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
    a.masterGain = 0.75f;
    a.masterFx.push_back(ReverbDesc(0.6f, 0.25f));   // master bus chain
    { EffectDesc pl; pl.type = EffectType::Plugin;    // plugin effect on master
      pl.pluginName = "Cool Plugin"; pl.params = {0.5f, 0.25f};
      a.masterFx.push_back(pl); }
    a.transport.playhead = 12345;
    a.transport.punchEnabled = true;
    a.transport.punchIn = 2000; a.transport.punchOut = 8000;

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gtr L"), a);
    TrackId t1 = a.Tracks().front().id;
    stack.Execute(std::make_unique<SetTrackGainCommand>(t1, 0.5f), a);
    stack.Execute(std::make_unique<SetTrackPanCommand>(t1, -0.3f), a);
    Clip c; c.startFrame = 1000; c.lengthFrames = 2000; c.sourceOffset = 10;
    c.sourcePath = "takes/one two.wav";   // note the space
    c.gain = 0.6f;                         // per-clip gain
    c.takeGroup = 7; c.takeActive = false; // loop-record take
    stack.Execute(std::make_unique<AddClipCommand>(t1, c), a);
    a.FindTrack(t1)->colorIndex = 3;   // per-track color + height
    a.FindTrack(t1)->height     = 104;
    a.tempoMap.sampleRate = a.sampleRate;
    a.tempoMap.Reset(a.tempoBPM, 4, 4);
    a.tempoMap.SetTempoAt(0, a.tempoBPM, true);   // frame-0 ramp flag
    a.tempoMap.SetTempoAt(48000, 90.0, true);     // a tempo change (ramp)
    a.tempoMap.SetMeterAt(96000, 3, 4);   // a meter change
    stack.Execute(std::make_unique<AddEffectCommand>(t1, LowPassDesc(700.0f)), a);
    stack.Execute(std::make_unique<AddEffectCommand>(t1,
        CompressorDesc(-18.0f, 3.0f, 5.0f, 80.0f, 6.0f)), a);   // p4 = makeup

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Synth"), a);
    TrackId t2 = a.Tracks().back().id;
    MidiNote n; n.pitch = 64; n.velocity = 90; n.startFrame = 500; n.lengthFrames = 480;
    stack.Execute(std::make_unique<AddNoteCommand>(t2, n), a);
    { Instrument in; in.waveform = 2; in.attack = 0.01f; in.decay = 0.2f;
      in.sustain = 0.5f; in.release = 0.3f; a.FindTrack(t2)->instrument = in; }

    // Aux send from track 1 to a bus (id t2 stands in as a dest here).
    { std::vector<Send> s = {{t2, 0.4f, true}};
      stack.Execute(std::make_unique<SetSendsCommand>(t1, s), a); }

    // Gain automation on track 1 (two breakpoints).
    { AutomationLane g; g.AddPoint(0, 1.0f); g.AddPoint(96000, 0.25f);
      stack.Execute(std::make_unique<SetAutoLaneCommand>(
          t1, AutoLaneKind::Gain, g), a); }
    { FxAutoLane fa; fa.fxIndex = 1; fa.slot = 4;   // effect-param automation
      fa.lane.AddPoint(0, -18.0f); fa.lane.AddPoint(48000, -6.0f);
      a.FindTrack(t1)->fxAuto.push_back(fa); }

    CHECK(ProjectIO::Save(a, path));

    Project b;
    CHECK(ProjectIO::Load(b, path));
    CHECK(b.sampleRate == 44100.0);
    CHECK(b.tempoBPM == 90.0);
    CHECK(std::abs(b.masterGain - 0.75f) < 1e-4f);
    CHECK(b.masterFx.size() == 2);
    CHECK(b.masterFx[0].type == EffectType::Reverb);
    CHECK(b.masterFx[1].type == EffectType::Plugin);
    CHECK(b.masterFx[1].pluginName == "Cool Plugin");
    CHECK(std::abs(b.masterFx[1].p(1) - 0.25f) < 1e-4f);
    CHECK(b.tempoMap.Tempos().size() == 2);          // frame-0 seed + change
    CHECK(b.tempoMap.Tempos()[0].ramp == true);      // frame-0 ramp persisted
    CHECK(b.tempoMap.Tempos()[1].frame == 48000);
    CHECK(b.tempoMap.Tempos()[1].ramp == true);      // change ramp persisted
    CHECK(std::abs(b.tempoMap.Tempos()[1].bpm - 90.0) < 1e-6);
    CHECK(b.tempoMap.Meters().size() == 2);
    CHECK(b.tempoMap.Meters()[1].frame == 96000);
    CHECK(b.tempoMap.Meters()[1].num == 3);
    CHECK(b.transport.playhead == 12345);
    CHECK(b.transport.punchEnabled == true);
    CHECK(b.transport.punchIn == 2000);
    CHECK(b.transport.punchOut == 8000);
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
    CHECK(std::abs(bt1.clips[0].gain - 0.6f) < 1e-4f);   // per-clip gain roundtrips
    CHECK(bt1.clips[0].takeGroup == 7);
    CHECK(bt1.clips[0].takeActive == false);
    CHECK(bt1.fx.size() == 2);
    CHECK(bt1.fx[0].type == EffectType::Biquad);
    CHECK(bt1.fx[1].type == EffectType::Compressor);
    CHECK(std::abs(bt1.fx[1].p(4) - 6.0f) < 1e-4f);   // makeup persisted
    CHECK(bt1.sends.size() == 1);
    CHECK(bt1.sends[0].dest == t2);
    CHECK(std::abs(bt1.sends[0].level - 0.4f) < 1e-4f);
    CHECK(bt1.sends[0].preFader == true);
    CHECK(bt1.fxAuto.size() == 1);
    CHECK(bt1.fxAuto[0].fxIndex == 1);
    CHECK(bt1.fxAuto[0].slot == 4);
    CHECK(bt1.fxAuto[0].lane.Count() == 2);
    CHECK(std::abs(bt1.fxAuto[0].lane.At(1).value - (-6.0f)) < 1e-4f);
    CHECK(bt1.gainAuto.Count() == 2);
    CHECK(bt1.gainAuto.At(0).frame == 0);
    CHECK(std::abs(bt1.gainAuto.At(1).value - 0.25f) < 1e-4f);
    CHECK(bt1.gainAuto.At(1).frame == 96000);
    CHECK(bt1.panAuto.Count() == 0);   // untouched lane stays empty
    CHECK(bt1.colorIndex == 3);
    CHECK(bt1.height == 104);

    const Track& bt2 = b.Tracks()[1];
    CHECK(bt2.type == TrackType::Midi);
    CHECK(bt2.name == "Synth");
    CHECK(bt2.notes.size() == 1);
    CHECK(bt2.notes[0].pitch == 64);
    CHECK(bt2.notes[0].startFrame == 500);
    CHECK(bt2.instrument.waveform == 2);
    CHECK(std::abs(bt2.instrument.decay - 0.2f) < 1e-4f);
    CHECK(std::abs(bt2.instrument.sustain - 0.5f) < 1e-4f);
    CHECK(std::abs(bt2.instrument.release - 0.3f) < 1e-4f);

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
    // Loading a non-DAW file (e.g. a WAV picked by mistake) must fail WITHOUT
    // clearing the current project.
    {
        const char* bogus = "not_a_project_tmp.bin";
        std::ofstream bf(bogus, std::ios::binary);
        bf << "RIFF";                          // WAV-like magic, no newline
        for (int i = 0; i < 2000; i++) bf.put((char)(i & 0xFF));
        bf.close();

        Project keep;
        CommandStack ks;
        ks.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Keep"), keep);
        CHECK(!ProjectIO::Load(keep, bogus));      // rejected
        CHECK(keep.Tracks().size() == 1);          // project untouched
        CHECK(keep.Tracks().front().name == "Keep");
        std::remove(bogus);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
