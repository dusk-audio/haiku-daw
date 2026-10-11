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
    stack.Execute(std::make_unique<AddEffectCommand>(t1,
        LimiterDesc(-2.0f, 4.0f, 90.0f, 3.0f)), a);   // type 9 must survive round-trip

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Synth"), a);
    TrackId t2 = a.Tracks().back().id;
    MidiClip mc; mc.startFrame = 2000; mc.lengthFrames = 4000;
    mc.fadeInFrames = 300; mc.fadeOutFrames = 450;
    { MidiNote n; n.pitch = 64; n.velocity = 90; n.startFrame = 500;   // clip-rel
      n.lengthFrames = 480; mc.notes.push_back(n); }
    mc.events.push_back({ MidiClipEvent::CC,        100, 11, 88 });     // clip-rel
    mc.events.push_back({ MidiClipEvent::PitchBend, 700,  0, 12000 });
    { auto add = std::make_unique<AddMidiClipCommand>(t2, mc);
      stack.Execute(std::move(add), a); }
    { Instrument in; in.waveform = 2; in.attack = 0.01f; in.decay = 0.2f;
      in.sustain = 0.5f; in.release = 0.3f; a.FindTrack(t2)->instrument.synth = in; }

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
    CHECK(bt1.fx.size() == 3);
    CHECK(bt1.fx[0].type == EffectType::Biquad);
    CHECK(bt1.fx[1].type == EffectType::Compressor);
    CHECK(std::abs(bt1.fx[1].p(4) - 6.0f) < 1e-4f);   // makeup persisted
    CHECK(bt1.fx[2].type == EffectType::Limiter);      // type 9 not clamped to Biquad
    CHECK(std::abs(bt1.fx[2].p(0) - (-2.0f)) < 1e-4f); // ceiling persisted
    CHECK(std::abs(bt1.fx[2].p(2) - 90.0f) < 1e-4f);   // release persisted
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
    CHECK(bt2.midiClips.size() == 1);
    CHECK(bt2.midiClips[0].startFrame == 2000);
    CHECK(bt2.midiClips[0].lengthFrames == 4000);
    CHECK(bt2.midiClips[0].fadeInFrames == 300);    // MIDI fades round-trip
    CHECK(bt2.midiClips[0].fadeOutFrames == 450);
    CHECK(bt2.midiClips[0].notes.size() == 1);
    CHECK(bt2.midiClips[0].notes[0].pitch == 64);
    CHECK(bt2.midiClips[0].notes[0].startFrame == 500);   // clip-relative
    CHECK(bt2.CollectNotes()[0].startFrame == 2500);       // absolute = 2000+500
    // MIDI clip events (CC + pitch bend) round-trip, clip-relative.
    CHECK(bt2.midiClips[0].events.size() == 2);
    CHECK(bt2.midiClips[0].events[0].type == MidiClipEvent::CC);
    CHECK(bt2.midiClips[0].events[0].data == 11);
    CHECK(bt2.midiClips[0].events[0].value == 88);
    CHECK(bt2.midiClips[0].events[0].startFrame == 100);
    CHECK(bt2.midiClips[0].events[1].type == MidiClipEvent::PitchBend);
    CHECK(bt2.midiClips[0].events[1].value == 12000);
    { auto ev = bt2.CollectEvents();                        // absolute-timeline
      CHECK(ev.size() == 2);
      CHECK(ev[0].startFrame == 2100);                       // 2000 + 100
      CHECK(ev[1].startFrame == 2700); }
    CHECK(bt2.instrument.synth.waveform == 2);
    CHECK(std::abs(bt2.instrument.synth.decay - 0.2f) < 1e-4f);
    CHECK(std::abs(bt2.instrument.synth.sustain - 0.5f) < 1e-4f);
    CHECK(std::abs(bt2.instrument.synth.release - 0.3f) < 1e-4f);

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

    // `mev` field ranges. Nothing downstream re-checks these: MidiChannelGain
    // divides the raw CC7/CC11 values by 127 with no clamp, so an out-of-range
    // value stored here is a large gain on that track's notes. An unknown TYPE
    // is different — that's forward compatibility, and must be skipped rather
    // than rejected.
    {
        const char* mp = "mev_range_tmp.dawproj";
        auto writeMev = [&](const char* mev) {
            std::ofstream mf(mp);
            mf << "DAW 1 1\n"
               << "track 1 midi 1 0 0 0 0 0 \"T\" 0 60 0 0 0\n"
               << "midiclip 1 0 48000 0 0 0 0 1\n"
               << mev << "\n"
               << "endtrack\nenddaw\n";
        };
        auto loads = [&](const char* mev) {
            writeMev(mev);
            Project q;
            const bool ok = ProjectIO::Load(q, mp);
            std::remove(mp);
            return ok;
        };
        auto eventCount = [&](const char* mev) -> size_t {
            writeMev(mev);
            Project q;
            const bool ok = ProjectIO::Load(q, mp);
            std::remove(mp);
            if (!ok || q.Tracks().empty() || q.Tracks()[0].midiClips.empty())
                return 0;
            return q.Tracks()[0].midiClips[0].events.size();
        };

        CHECK(loads("mev 0 100 11 88"));            // in-range CC
        CHECK(eventCount("mev 0 100 11 88") == 1);
        CHECK(loads("mev 1 100 0 12000"));          // in-range pitch bend
        CHECK(eventCount("mev 1 100 0 12000") == 1);
        CHECK(loads("mev 1 100 0 16383"));          // bend upper bound

        CHECK(!loads("mev 0 100 11 65535"));        // CC value past 127
        CHECK(!loads("mev 0 100 11 -5"));           // negative CC value
        CHECK(!loads("mev 0 100 9999 64"));         // CC number past 127
        CHECK(!loads("mev 0 -100 11 64"));          // negative clip-relative frame
        CHECK(!loads("mev 1 100 0 16384"));         // bend past 14 bits
        CHECK(!loads("mev 0 100"));                 // truncated record

        // An unrecognised type is skipped, not rejected: a newer build may
        // define more, and this build simply has nothing to do with it.
        CHECK(loads("mev 9 100 11 64"));
        CHECK(eventCount("mev 9 100 11 64") == 0);
    }

    // Markers: add (kept sorted) / rename / remove commands, undo, IO round-trip.
    {
        Project p;
        CommandStack s;
        s.Execute(std::make_unique<AddMarkerCommand>(2000, "B"), p);
        s.Execute(std::make_unique<AddMarkerCommand>(1000, "A"), p);   // earlier
        CHECK(p.markers.size() == 2);
        CHECK(p.markers[0].frame == 1000 && p.markers[0].name == "A"); // sorted
        CHECK(p.markers[1].frame == 2000 && p.markers[1].name == "B");
        s.Execute(std::make_unique<RenameMarkerCommand>(1000, "Intro"), p);
        CHECK(p.markers[0].name == "Intro");
        s.Undo(p);   // undo rename
        CHECK(p.markers[0].name == "A");
        s.Execute(std::make_unique<RemoveMarkerCommand>(1000), p);
        CHECK(p.markers.size() == 1 && p.markers[0].frame == 2000);
        s.Undo(p);   // undo remove -> back at index 0
        CHECK(p.markers.size() == 2 && p.markers[0].frame == 1000);

        // Two markers sharing a frame: remove/rename must hit the one named,
        // not just the first at that frame (marker identity = frame + name).
        {
            Project pc;
            CommandStack sc;
            sc.Execute(std::make_unique<AddMarkerCommand>(5000, "Verse"), pc);
            sc.Execute(std::make_unique<AddMarkerCommand>(5000, "Chorus"), pc);
            CHECK(pc.markers.size() == 2);
            // Rename the second-at-frame ("Chorus") specifically.
            sc.Execute(std::make_unique<RenameMarkerCommand>(
                (Frame)5000, std::string("Chorus"), std::string("Bridge")), pc);
            bool haveVerse = false, haveBridge = false, haveChorus = false;
            for (const Marker& m : pc.markers) {
                if (m.name == "Verse")  haveVerse  = true;
                if (m.name == "Bridge") haveBridge = true;
                if (m.name == "Chorus") haveChorus = true;
            }
            CHECK(haveVerse && haveBridge && !haveChorus);   // only Chorus renamed
            sc.Undo(pc);                                      // restores "Chorus"
            haveBridge = haveChorus = false;
            for (const Marker& m : pc.markers) {
                if (m.name == "Bridge") haveBridge = true;
                if (m.name == "Chorus") haveChorus = true;
            }
            CHECK(haveChorus && !haveBridge);
            // Remove "Verse" by name; "Chorus" must survive.
            sc.Execute(std::make_unique<RemoveMarkerCommand>(
                (Frame)5000, std::string("Verse")), pc);
            CHECK(pc.markers.size() == 1 && pc.markers[0].name == "Chorus");
            sc.Undo(pc);                                      // Verse back
            CHECK(pc.markers.size() == 2);
        }

        const char* path = "markers_tmp.dawproj";
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        std::remove(path);
        CHECK(q.markers.size() == 2);
        CHECK(q.markers[0].frame == 1000 && q.markers[0].name == "A");
        CHECK(q.markers[1].frame == 2000 && q.markers[1].name == "B");
    }

    // Version gate: a future major version must be rejected, project untouched,
    // rather than silently misread by the v1 parser.
    {
        const char* vpath = "future_ver_tmp.dawproj";
        std::ofstream vf(vpath);
        vf << "DAW 2\nsampleRate 96000\nenddaw\n";
        vf.close();
        Project keep;
        CommandStack ks;
        ks.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Keep"), keep);
        CHECK(!ProjectIO::Load(keep, vpath));       // rejected
        CHECK(keep.Tracks().size() == 1);           // untouched
        CHECK(keep.sampleRate == 48000.0);          // not overwritten with 96000
        std::remove(vpath);
    }

    // Corrupt-file DoS: a huge count must not drive an unbounded allocation or
    // hang; the loader caps/streams and completes.
    {
        const char* dpath = "huge_count_tmp.dawproj";
        std::ofstream df(dpath);
        df << "DAW 1\n"
           << "track 1 audio 1.0 0.0 0 0 0\n"
           << "auto gain 2000000000\n"       // no data tokens follow
           << "endtrack\nenddaw\n";
        df.close();
        Project d;
        CHECK(ProjectIO::Load(d, dpath));               // completes, no OOM/hang
        CHECK(d.Tracks().size() == 1);
        CHECK(d.Tracks()[0].gainAuto.Count() == 0);     // nothing bogus added
        std::remove(dpath);
    }

    // String escaping + inputMonitor: a track name and clip path containing a
    // double-quote and a newline must round-trip without corrupting the line,
    // and inputMonitor must persist.
    {
        Project a;
        CommandStack s;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Gt\"r\nL"), a);
        TrackId t = a.Tracks().front().id;
        a.FindTrack(t)->inputMonitor = true;
        Clip c; c.startFrame = 0; c.lengthFrames = 10;
        c.sourcePath = "takes/od\"d name.wav";       // embedded quote in path
        s.Execute(std::make_unique<AddClipCommand>(t, c), a);

        const char* path = "escape_tmp.dawproj";
        CHECK(ProjectIO::Save(a, path));
        Project b;
        CHECK(ProjectIO::Load(b, path));
        std::remove(path);
        CHECK(b.Tracks().size() == 1);               // line not split by the \n
        CHECK(b.Tracks()[0].name == "Gt\"r\nL");     // exact name survives
        CHECK(b.Tracks()[0].inputMonitor == true);   // persisted
        CHECK(b.Tracks()[0].clips.size() == 1);
        CHECK(b.Tracks()[0].clips[0].sourcePath == "takes/od\"d name.wav");
    }

    // Save precision: a gain that the default 6-digit stream would round must
    // round-trip within float epsilon.
    {
        Project a;
        CommandStack s;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "P"), a);
        TrackId t = a.Tracks().front().id;
        s.Execute(std::make_unique<SetTrackGainCommand>(t, 0.123456789f), a);
        const char* path = "prec_tmp.dawproj";
        CHECK(ProjectIO::Save(a, path));
        Project b; CHECK(ProjectIO::Load(b, path));
        std::remove(path);
        CHECK(std::abs(b.Tracks()[0].gain - 0.123456789f) < 1e-6f);
    }

    // Atomic save leaves no ".tmp" sidecar once it succeeds.
    {
        const char* apath = "atomic_tmp.dawproj";
        Project p;
        CHECK(ProjectIO::Save(p, apath));
        const std::string sidecar = std::string(apath) + ".tmp";
        std::ifstream leftover(sidecar);
        CHECK(!leftover.good());                        // renamed away
        std::remove(apath);
    }

    // Clip depth (M2.5): name / colour / mute / fade shape round-trip, on both
    // an audio clip and a MIDI region (the mute line is shared).
    {
        Project a;
        CommandStack s;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "C"), a);
        TrackId t = a.Tracks().front().id;
        Clip c; c.startFrame = 0; c.lengthFrames = 100;
        c.sourcePath = "x.wav";
        c.name = "Verse \"one\"";   // spaces + a quote: the quoted token must hold
        c.colorIndex = 4;
        c.muted = true;
        c.fadeShape = FadeShape::SCurve;
        s.Execute(std::make_unique<AddClipCommand>(t, c), a);
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "M"), a);
        TrackId tm = a.Tracks().back().id;
        MidiClip mc; mc.startFrame = 0; mc.lengthFrames = 100;
        mc.muted = true;
        s.Execute(std::make_unique<AddMidiClipCommand>(tm, mc), a);

        const char* path = "clipdepth_tmp.dawproj";
        CHECK(ProjectIO::Save(a, path));
        Project b;
        CHECK(ProjectIO::Load(b, path));

        const Clip& bc = b.Tracks()[0].clips[0];
        CHECK(bc.name == "Verse \"one\"");
        CHECK(bc.colorIndex == 4);
        CHECK(bc.muted == true);
        CHECK(bc.fadeShape == FadeShape::SCurve);
        CHECK(b.Tracks()[1].midiClips[0].muted == true);

        // Compat, direction 1: the CLIP LINE itself is byte-identical to the
        // previous format — every new field rides on its own line, so a reader
        // that does not know them still parses gain/takeGroup/takeActive from
        // the tail exactly as it did.
        {
            std::ifstream in(path);
            std::string line, clipLine, midiLine;
            int infoLines = 0, muteLines = 0;
            while (std::getline(in, line)) {
                if (line.rfind("clipinfo ", 0) == 0) infoLines++;
                if (line.rfind("clipmute ", 0) == 0) muteLines++;
                if (line.rfind("clip ", 0) == 0) clipLine = line;
                if (line.rfind("midiclip ", 0) == 0) midiLine = line;
            }
            CHECK(infoLines == 1);
            CHECK(muteLines == 2);   // the audio clip and the MIDI region
            CHECK(clipLine == "clip 1 0 100 0 0 0 \"x.wav\" 1 0 1");
            CHECK(midiLine == "midiclip 2 0 100 0 0 0 0 1");
        }
        std::remove(path);
    }

    // Compat, direction 2: an old file (no clipinfo/clipmute at all) loads with
    // the documented defaults, and a project that never touches the new fields
    // writes NO new lines — its clip line is the legacy spelling, unchanged.
    {
        const char* path = "legacyclip_tmp.dawproj";
        {
            std::ofstream out(path);
            out << "DAW 1 1\n"
                << "sampleRate 48000\n"
                << "tempo 120 0\n"
                << "master 1\n"
                << "timesig 4 4\n"
                << "track 1 audio 1 0 0 0 0 \"Old\" 0 72 0 0 0\n"
                << "clip 1 0 4800 0 0 0 \"old.wav\" 0.5 0 1\n"
                << "midiclip 2 0 4800 0 0 0 0 1\n"
                << "note 60 100 0 480\n"
                << "enddaw\n";
        }
        Project p;
        CHECK(ProjectIO::Load(p, path));
        CHECK(p.Tracks().size() == 1);
        const Clip& c = p.Tracks()[0].clips[0];
        CHECK(std::abs(c.gain - 0.5f) < 1e-4f);   // the old tail still parses
        CHECK(c.name.empty());
        CHECK(c.colorIndex == 0);
        CHECK(c.muted == false);
        CHECK(c.fadeShape == FadeShape::Linear);
        CHECK(!p.Tracks()[0].midiClips.empty());
        CHECK(p.Tracks()[0].midiClips[0].muted == false);
        std::remove(path);

        // The save side of the same rule.
        Project q;
        CommandStack s;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Q"), q);
        Clip c2; c2.startFrame = 0; c2.lengthFrames = 4800;
        c2.sourcePath = "old.wav"; c2.gain = 0.5f;
        c2.takeGroup = 2;
        s.Execute(std::make_unique<AddClipCommand>(q.Tracks().front().id, c2), q);
        CHECK(ProjectIO::Save(q, path));
        {
            std::ifstream in(path);
            std::string line, clipLine;
            int newLines = 0;
            while (std::getline(in, line)) {
                if (line.rfind("clipinfo ", 0) == 0
                    || line.rfind("clipmute ", 0) == 0) newLines++;
                if (line.rfind("clip ", 0) == 0) clipLine = line;
            }
            CHECK(newLines == 0);
            CHECK(clipLine == "clip 1 0 4800 0 0 0 \"old.wav\" 0.5 2 1");
        }
        std::remove(path);
    }

    // A truncated clipinfo line is skipped, not half-applied: the clip keeps
    // what its own line said.
    {
        const char* path = "truncinfo_tmp.dawproj";
        {
            std::ofstream out(path);
            out << "DAW 1 1\n"
                << "sampleRate 48000\n"
                << "tempo 120 0\n"
                << "master 1\n"
                << "timesig 4 4\n"
                << "track 1 audio 1 0 0 0 0 \"T\" 0 72 0 0 0\n"
                << "clip 1 0 100 0 0 0 \"a.wav\" 1 0 1\n"
                << "clipinfo 1 3\n"                  // missing shape + name
                << "clipmute 1\n"                    // missing the flag
                << "enddaw\n";
        }
        Project p;
        CHECK(ProjectIO::Load(p, path));
        const Clip& c = p.Tracks()[0].clips[0];
        CHECK(c.colorIndex == 0 && c.name.empty() && !c.muted);
        std::remove(path);
    }

    // Track depth (M2.6): a folded lane round-trips on its own optional line,
    // an old file loads expanded, and a project that folds nothing writes the
    // legacy track bytes exactly.
    {
        Project a;
        CommandStack s;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Fold"), a);
        TrackId t = a.Tracks().front().id;
        s.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Open"), a);
        s.Execute(std::make_unique<SetTrackCollapsedCommand>(t, true), a);

        const char* path = "trackfold_tmp.dawproj";
        CHECK(ProjectIO::Save(a, path));
        Project b;
        CHECK(ProjectIO::Load(b, path));
        CHECK(b.Tracks().size() == 2);
        CHECK(b.Tracks()[0].collapsed == true);
        CHECK(b.Tracks()[1].collapsed == false);
        std::remove(path);

        // The save side: the folded track writes the line, the other does not,
        // and the track line itself is untouched.
        CHECK(ProjectIO::Save(a, path));
        {
            std::ifstream in(path);
            std::string line, foldLine, firstTrack;
            int folds = 0;
            while (std::getline(in, line)) {
                if (line.rfind("trackfold ", 0) == 0) { folds++; foldLine = line; }
                else if (line.rfind("track ", 0) == 0 && firstTrack.empty())
                    firstTrack = line;
            }
            CHECK(folds == 1);
            CHECK(foldLine == "trackfold 1 1");
            // The track line itself is unchanged: the fold is a separate line,
            // never a new tail field.
            CHECK(firstTrack.rfind("track 1 audio ", 0) == 0);
            CHECK(firstTrack.find("\"Fold\" 0 72 0 0 0") != std::string::npos);
        }
        std::remove(path);
    }

    // Compat: a file from before the fold existed loads with every lane
    // expanded, and an unknown/truncated trackfold is skipped rather than
    // guessing.
    {
        const char* path = "legacyfold_tmp.dawproj";
        {
            std::ofstream out(path);
            out << "DAW 1 1\n"
                << "sampleRate 48000\n"
                << "tempo 120 0\n"
                << "master 1\n"
                << "timesig 4 4\n"
                << "track 1 audio 1 0 0 0 0 \"Old\" 0 72 0 0 0\n"
                << "track 2 midi 1 0 0 0 0 \"Two\" 0 72 0 0 0\n"
                << "trackfold 2\n"       // truncated: skipped
                << "trackfold 9 1\n"     // unknown track id: no effect
                << "enddaw\n";
        }
        Project p;
        CHECK(ProjectIO::Load(p, path));
        CHECK(p.Tracks().size() == 2);
        if (p.Tracks().size() == 2) {
            CHECK(p.Tracks()[0].collapsed == false);
            CHECK(p.Tracks()[1].collapsed == false);
        }
        std::remove(path);
    }

    // Fold lines are addressed BY ID and applied after the whole file: one
    // written after a later track still folds the track it names, and one
    // written before its own record is not lost either.
    {
        const char* path = "foldorder_tmp.dawproj";
        {
            std::ofstream out(path);
            out << "DAW 1 1\n"
                << "sampleRate 48000\n"
                << "tempo 120 0\n"
                << "master 1\n"
                << "timesig 4 4\n"
                << "trackfold 2 1\n"     // for a track not read yet
                << "track 1 audio 1 0 0 0 0 \"One\" 0 72 0 0 0\n"
                << "track 2 audio 1 0 0 0 0 \"Two\" 0 72 0 0 0\n"
                << "trackfold 1 1\n"     // for a track already committed
                << "enddaw\n";
        }
        Project p;
        CHECK(ProjectIO::Load(p, path));
        CHECK(p.Tracks().size() == 2);
        if (p.Tracks().size() == 2) {
            CHECK(p.Tracks()[0].collapsed == true);
            CHECK(p.Tracks()[1].collapsed == true);
        }
        std::remove(path);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
