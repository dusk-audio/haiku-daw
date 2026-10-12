// Host tests for the Project Settings model work (M3.6): the meter and notes
// commands, the notes line's format compatibility, the timeline rescale a
// sample-rate change performs (over every frame-bearing field), the command's
// exact undo, and the media conversion's own output.
//
// The rescale is the one that matters most: frames ARE samples at the project's
// rate, so a field this forgets slides in time -- a bug no screenshot shows.
#include "../src/model/Commands.h"
#include "../src/model/Project.h"
#include "../src/model/ProjectIO.h"
#include "../src/model/ProjectResample.h"
#include "../src/model/ProjectTimebase.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static std::string TempPath(const char* name) {
    return std::string("/tmp/haiku_daw_ps_") + name;
}

// A project with every frame-bearing field filled in at a known value, so a
// rescale can be checked field by field.
static Project MakeFrameRichProject() {
    Project p;
    p.sampleRate = 48000.0;
    p.tempoBPM = 120.0;
    p.tempoMap.sampleRate = 48000.0;
    p.tempoMap.SetTempoAt(96000, 90.0, false);
    p.tempoMap.SetMeterAt(48000, 3, 4);
    p.transport.playhead = 24000;
    p.transport.loopEnabled = true;
    p.transport.loopStart = 12000;
    p.transport.loopEnd = 60000;
    p.transport.punchEnabled = true;
    p.transport.punchIn = 6000;
    p.transport.punchOut = 30000;
    p.markers.push_back(Marker{ 12000, "intro" });

    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Audio;
    t.name = "A";
    t.gainAuto.AddPoint(0, 1.0f);
    t.gainAuto.AddPoint(24000, 0.5f);
    t.panAuto.AddPoint(12000, -0.5f);
    FxAutoLane lane; lane.fxIndex = 0; lane.slot = 1;
    lane.lane.AddPoint(6000, 0.25f);
    t.fxAuto.push_back(lane);

    Clip c;
    c.id = p.NextClipId();
    c.startFrame = 12000;
    c.lengthFrames = 24000;
    c.sourceOffset = 3000;
    c.fadeInFrames = 1200;
    c.fadeOutFrames = 600;
    c.sourcePath = "media/take.wav";
    t.clips.push_back(c);

    MidiClip m;
    m.id = p.NextClipId();
    m.startFrame = 48000;
    m.lengthFrames = 12000;
    m.fadeInFrames = 300;
    m.fadeOutFrames = 150;
    m.notes.push_back(MidiNote{ 60, 100, 1000, 2000 });
    m.events.push_back(MidiClipEvent{ MidiClipEvent::CC, 500, 64, 127 });
    t.midiClips.push_back(m);

    // The frozen copy a frozen track carries has to scale with the rest.
    Clip fc = c;
    fc.id = p.NextClipId();
    fc.startFrame = 36000;
    t.freezeClips.push_back(fc);
    t.frozen = true;

    p.AddTrack(t);
    return p;
}

int main() {
    // --- SetMeterCommand ----------------------------------------------------
    {
        Project p;
        CommandStack stack;
        CHECK(p.timeSig.numerator == 4 && p.timeSig.denominator == 4);
        CHECK(stack.Execute(std::make_unique<SetMeterCommand>(6, 8), p));
        CHECK(p.timeSig.numerator == 6 && p.timeSig.denominator == 8);
        // Both halves of the fact: the map's frame-0 meter moved too.
        CHECK(p.tempoMap.Meters().front().num == 6);
        CHECK(p.tempoMap.Meters().front().denom == 8);
        CHECK(stack.Undo(p));
        CHECK(p.timeSig.numerator == 4 && p.timeSig.denominator == 4);
        CHECK(p.tempoMap.Meters().front().num == 4);
        CHECK(stack.Redo(p));
        CHECK(p.timeSig.numerator == 6);
        // A nonsense meter is clamped instead of stored (the engine and the
        // meter maths divide by the beat count).
        CHECK(stack.Execute(std::make_unique<SetMeterCommand>(0, 0), p));
        CHECK(p.timeSig.numerator == 1 && p.timeSig.denominator == 1);
        CHECK(p.tempoMap.Meters().front().num == 1);
    }

    // --- SetProjectNotesCommand --------------------------------------------
    {
        Project p;
        CommandStack stack;
        CHECK(stack.Execute(std::make_unique<SetProjectNotesCommand>(
                                "Take 3 is the keeper\n(louder bridge)"), p));
        CHECK(p.notes == "Take 3 is the keeper\n(louder bridge)");
        CHECK(stack.IsDirty());
        CHECK(stack.Undo(p));
        CHECK(p.notes.empty());
        CHECK(stack.Redo(p));
        CHECK(p.notes == "Take 3 is the keeper\n(louder bridge)");
    }

    // --- the notes line's format compatibility -----------------------------
    {
        const std::string path = TempPath("notes.dawproj");
        Project p;
        p.notes = "bridge on 2; keep the \"live\" take\nsecond line";
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        CHECK(q.notes == p.notes);          // newlines + quotes survive

        // A project with no notes writes no `notes` line at all (the
        // append-only rule: an untouched project stays byte-identical).
        Project empty;
        const std::string emptyPath = TempPath("empty.dawproj");
        CHECK(ProjectIO::Save(empty, emptyPath));
        std::ifstream f(emptyPath);
        std::string line, all;
        while (std::getline(f, line)) { all += line; all += "\n"; }
        CHECK(all.find("notes") == std::string::npos);

        // ... and a file from before notes existed loads with none, not with
        // garbage: an older project is a normal project.
        Project before;
        CHECK(ProjectIO::Load(before, emptyPath));
        CHECK(before.notes.empty());

        // An unknown line from a NEWER build is ignored, the rest still loads.
        const std::string futurePath = TempPath("future.dawproj");
        {
            std::ofstream out(futurePath, std::ios::trunc);
            out << "DAW 1 1\n"
                << "sampleRate 44100\n"
                << "tempo 140 0\n"
                << "somedayfield 7 whatever\n"
                << "notes \"from the future\"\n";
        }
        Project future;
        CHECK(ProjectIO::Load(future, futurePath));
        CHECK(std::fabs(future.sampleRate - 44100.0) < 1e-9);
        CHECK(future.notes == "from the future");
        std::remove(path.c_str());
        std::remove(emptyPath.c_str());
        std::remove(futurePath.c_str());
    }

    // --- ScaleProjectTimebase: every frame-bearing field -------------------
    {
        Project p = MakeFrameRichProject();
        // 48k -> 96k is exactly a factor of two, so every expectation is exact
        // and a rounding mistake cannot hide.
        ScaleProjectTimebase(p, 2.0);
        CHECK(p.transport.playhead == 48000);
        CHECK(p.transport.loopStart == 24000 && p.transport.loopEnd == 120000);
        CHECK(p.transport.punchIn == 12000 && p.transport.punchOut == 60000);
        CHECK(p.markers.size() == 1 && p.markers[0].frame == 24000);
        // The map: both change anchors moved, the frame-0 anchors stayed.
        CHECK(p.tempoMap.Tempos().size() == 2);
        CHECK(p.tempoMap.Tempos()[0].frame == 0);
        CHECK(p.tempoMap.Tempos()[1].frame == 192000);
        CHECK(p.tempoMap.Meters().size() == 2);
        CHECK(p.tempoMap.Meters()[1].frame == 96000);
        CHECK(p.tempoMap.Meters()[1].num == 3);   // the meter's NUMBERS did not
        const Track& t = p.Tracks()[0];           // ... only its anchor moved
        CHECK(t.clips.size() == 1);
        CHECK(t.clips[0].startFrame == 24000 && t.clips[0].lengthFrames == 48000);
        CHECK(t.clips[0].sourceOffset == 6000);
        CHECK(t.clips[0].fadeInFrames == 2400 && t.clips[0].fadeOutFrames == 1200);
        CHECK(t.clips[0].sourcePath == "media/take.wav");   // paths are not time
        CHECK(t.midiClips.size() == 1);
        CHECK(t.midiClips[0].startFrame == 96000);
        CHECK(t.midiClips[0].lengthFrames == 24000);
        CHECK(t.midiClips[0].fadeInFrames == 600);
        CHECK(t.midiClips[0].notes.size() == 1);
        CHECK(t.midiClips[0].notes[0].startFrame == 2000);
        CHECK(t.midiClips[0].notes[0].lengthFrames == 4000);
        CHECK(t.midiClips[0].events.size() == 1);
        CHECK(t.midiClips[0].events[0].startFrame == 1000);
        CHECK(t.freezeClips.size() == 1);
        CHECK(t.freezeClips[0].startFrame == 72000);
        CHECK(t.freezeClips[0].lengthFrames == 48000);
        // Automation breakpoints are timeline frames too.
        CHECK(t.gainAuto.Count() == 2);
        CHECK(t.gainAuto.At(1).frame == 48000);
        CHECK(std::fabs(t.gainAuto.At(1).value - 0.5f) < 1e-6);
        CHECK(t.panAuto.Count() == 1 && t.panAuto.At(0).frame == 24000);
        CHECK(t.fxAuto.size() == 1 && t.fxAuto[0].lane.At(0).frame == 12000);

        // Non-time fields were left alone.
        CHECK(t.gain == 1.0f && t.name == "A");
        CHECK(p.tempoBPM == 120.0);   // tempo is not a frame

        // And the round trip: scaling back by the inverse restores every frame
        // exactly (2.0 is exact in binary, so there is nothing to round).
        ScaleProjectTimebase(p, 0.5);
        Project orig = MakeFrameRichProject();
        const Track& back = p.Tracks()[0];
        CHECK(p.transport.playhead == orig.transport.playhead);
        CHECK(p.transport.loopEnd == orig.transport.loopEnd);
        CHECK(p.markers[0].frame == orig.markers[0].frame);
        CHECK(p.tempoMap.Tempos()[1].frame == orig.tempoMap.Tempos()[1].frame);
        CHECK(p.tempoMap.Meters()[1].frame == orig.tempoMap.Meters()[1].frame);
        CHECK(back.clips[0].startFrame == 12000);
        CHECK(back.clips[0].fadeOutFrames == 600);
        CHECK(back.midiClips[0].notes[0].startFrame == 1000);
        CHECK(back.freezeClips[0].startFrame == 36000);
        CHECK(back.gainAuto.At(1).frame == 24000);

        // A ratio of 1 (or a nonsense one) changes nothing at all.
        Project same = MakeFrameRichProject();
        ScaleProjectTimebase(same, 1.0);
        ScaleProjectTimebase(same, 0.0);
        ScaleProjectTimebase(same, -2.0);
        CHECK(same.Tracks()[0].clips[0].startFrame == 12000);
        CHECK(same.transport.playhead == 24000);
    }

    // --- SetSampleRateCommand ----------------------------------------------
    {
        Project p = MakeFrameRichProject();
        CommandStack stack;
        // No media conversion: the timeline rescales, the clip paths stay.
        CHECK(stack.Execute(std::make_unique<SetSampleRateCommand>(96000.0,
                                                                   false), p));
        CHECK(std::fabs(p.sampleRate - 96000.0) < 1e-9);
        CHECK(std::fabs(p.tempoMap.sampleRate - 96000.0) < 1e-9);
        CHECK(p.Tracks()[0].clips[0].startFrame == 24000);
        CHECK(p.Tracks()[0].clips[0].sourcePath == "media/take.wav");
        CHECK(stack.IsDirty());
        // Undo is exact, and redo does not double-apply the ratio.
        CHECK(stack.Undo(p));
        CHECK(std::fabs(p.sampleRate - 48000.0) < 1e-9);
        CHECK(p.Tracks()[0].clips[0].startFrame == 12000);
        CHECK(p.Tracks()[0].clips[0].lengthFrames == 24000);
        CHECK(p.transport.playhead == 24000);
        CHECK(stack.Redo(p));
        CHECK(std::fabs(p.sampleRate - 96000.0) < 1e-9);
        CHECK(p.Tracks()[0].clips[0].startFrame == 24000);

        // The same rate is a no-op on the timeline (ratio 1).
        Project q = MakeFrameRichProject();
        CommandStack s2;
        CHECK(s2.Execute(std::make_unique<SetSampleRateCommand>(48000.0, false), q));
        CHECK(q.Tracks()[0].clips[0].startFrame == 12000);

        // A nonsense rate is refused (and is not pushed onto the stack).
        Project r = MakeFrameRichProject();
        CommandStack s3;
        CHECK(!s3.Execute(std::make_unique<SetSampleRateCommand>(0.0, false), r));
        CHECK(std::fabs(r.sampleRate - 48000.0) < 1e-9);
    }

    // --- ResampleMediaFile --------------------------------------------------
    {
        // A 1 s 440 Hz tone at 48 kHz, written with the real writer.
        const std::string src = TempPath("tone.wav");
        const int srcRate = 48000, frames = srcRate;   // exactly 1 s
        {
            WavWriter w;
            CHECK(w.OpenFormat(src, srcRate, 2, 32, true));
            std::vector<float> buf(frames * 2);
            for (int i = 0; i < frames; i++) {
                const float s = (float)(0.5 * std::sin(2.0 * M_PI * 440.0
                                                       * i / srcRate));
                buf[i * 2] = buf[i * 2 + 1] = s;
            }
            CHECK(w.WriteFloat(buf.data(), buf.size(), false));
            CHECK(w.Close());
        }
        const std::string out = ResampleMediaFile(src, 96000.0);
        CHECK(!out.empty());
        CHECK(out == TempPath("tone-96k.wav"));   // named for its rate, beside it
        // The result: twice the frames (same 1 s), the tone still 440 Hz.
        WavSource in;
        CHECK(in.Open(out));
        CHECK(in.IsValid());
        CHECK(std::fabs(in.FrameRate() - 96000.0) < 1e-3);
        CHECK(in.TotalFrames() >= 95000 && in.TotalFrames() <= 96500);
        // A zero crossing count is the cheap frequency check: 440 Hz over 1 s
        // is ~880 crossings either way, and a wrong-rate copy would halve or
        // double that.
        int crossings = 0;
        const float* chunk = nullptr;
        size_t chunkFrames = 0;
        float prev = 0.0f;
        while (in.ReadChunk(&chunk, &chunkFrames)) {
            for (size_t i = 0; i < chunkFrames; i++) {
                const float v = chunk[i * 2];
                if (prev <= 0.0f && v > 0.0f) crossings++;
                prev = v;
            }
        }
        std::printf("  upward zero crossings: %d\n", crossings);
        CHECK(crossings >= 430 && crossings <= 450);   // 440 Hz = 440 per second

        // The source file is untouched, and a second conversion replaces its
        // own earlier output rather than writing a new name.
        WavSource orig;
        CHECK(orig.Open(src));
        CHECK(orig.IsValid());
        CHECK(orig.TotalFrames() == frames);
        const std::string again = ResampleMediaFile(src, 96000.0);
        CHECK(again == out);

        // Unknown/garbage input fails cleanly, with a reason and no file.
        std::string err;
        CHECK(ResampleMediaFile(TempPath("does-not-exist.wav"), 96000.0,
                                &err).empty());
        CHECK(!err.empty());
        const std::string junk = TempPath("junk.wav");
        {
            std::ofstream out2(junk, std::ios::trunc | std::ios::binary);
            out2 << "this is not audio at all, not even close";
        }
        err.clear();
        CHECK(ResampleMediaFile(junk, 96000.0, &err).empty());
        CHECK(!err.empty());
        std::remove(src.c_str());
        std::remove(out.c_str());
        std::remove(junk.c_str());
    }

    // --- the resample rides inside the command ------------------------------
    {
        const std::string src = TempPath("cmd-tone.wav");
        {
            WavWriter w;
            CHECK(w.OpenFormat(src, 48000, 2, 32, true));
            std::vector<float> buf(48000 * 2, 0.25f);   // 1 s of DC
            CHECK(w.WriteFloat(buf.data(), buf.size(), false));
            CHECK(w.Close());
        }
        Project p;
        p.sampleRate = 48000.0;
        p.tempoMap.sampleRate = 48000.0;
        Track t;
        t.id = p.NextTrackId();
        Clip c;
        c.id = p.NextClipId();
        c.startFrame = 0;
        c.lengthFrames = 48000;
        c.sourcePath = src;
        t.clips.push_back(c);
        p.AddTrack(t);

        CommandStack stack;
        auto cmd = std::make_unique<SetSampleRateCommand>(96000.0, true);
        SetSampleRateCommand* raw = cmd.get();
        CHECK(stack.Execute(std::move(cmd), p));
        // The clip follows the new file, and its length doubled with the rate.
        CHECK(p.Tracks()[0].clips[0].sourcePath == TempPath("cmd-tone-96k.wav"));
        CHECK(p.Tracks()[0].clips[0].lengthFrames == 96000);
        CHECK(raw->Resampled().size() == 1);
        CHECK(raw->Resampled()[0].second == TempPath("cmd-tone-96k.wav"));
        // ... and the new file is real, at the new rate.
        WavSource made;
        CHECK(made.Open(TempPath("cmd-tone-96k.wav")));
        CHECK(made.IsValid());
        CHECK(std::fabs(made.FrameRate() - 96000.0) < 1e-3);
        CHECK(made.TotalFrames() >= 95000 && made.TotalFrames() <= 96500);

        // Undo puts the clips back on the original files (the converted file
        // stays on disk; nothing points at it).
        CHECK(stack.Undo(p));
        CHECK(p.Tracks()[0].clips[0].sourcePath == src);
        CHECK(p.Tracks()[0].clips[0].lengthFrames == 48000);
        CHECK(std::fabs(p.sampleRate - 48000.0) < 1e-9);
        std::remove(src.c_str());
        std::remove(TempPath("cmd-tone-96k.wav").c_str());
    }

    // --- a whole-project round trip through the file ------------------------
    {
        const std::string path = TempPath("roundtrip.dawproj");
        Project p = MakeFrameRichProject();
        p.notes = "settings survive a save";
        CommandStack stack;
        CHECK(stack.Execute(std::make_unique<SetSampleRateCommand>(96000.0,
                                                                   false), p));
        CHECK(stack.Execute(std::make_unique<SetMeterCommand>(7, 8), p));
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        CHECK(std::fabs(q.sampleRate - 96000.0) < 1e-9);
        CHECK(q.notes == "settings survive a save");
        CHECK(q.timeSig.numerator == 7 && q.timeSig.denominator == 8);
        CHECK(q.tempoMap.Meters().front().num == 7);
        CHECK(q.Tracks()[0].clips[0].startFrame == 24000);
        CHECK(q.tempoMap.Tempos()[1].frame == 192000);
        std::remove(path.c_str());
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
