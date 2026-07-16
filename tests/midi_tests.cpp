// Host tests for the kit-free MIDI core: the SPSC event ring and the
// MidiRecorder note-pairing (live events -> model MidiClip).
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/midi_tests.cpp src/midi/MidiRecorder.cpp \
//       src/model/Project.cpp -o /tmp/midi && /tmp/midi
#include "../src/midi/MidiEventRing.h"
#include "../src/midi/MidiRecorder.h"
#include "../src/model/Commands.h"
#include "../src/model/ProjectIO.h"

#include <cstdio>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    // --- MidiEvent helpers ---------------------------------------------------
    {
        MidiEvent on = MidiEvent::NoteOn(0, 60, 100);
        CHECK(on.type == MidiEvent::kNoteOn && on.IsNoteOn() && !on.IsNoteOff());
        // Note-on with velocity 0 is a note-off (running-status convention).
        MidiEvent z = MidiEvent::NoteOn(0, 60, 0);
        CHECK(z.type == MidiEvent::kNoteOff && z.IsNoteOff());
        MidiEvent off = MidiEvent::NoteOff(3, 64, 0);
        CHECK(off.channel == 3 && off.data1 == 64 && off.IsNoteOff());
    }

    // --- MidiEventRing: FIFO, wrap, overflow drops newest --------------------
    {
        MidiEventRing ring(4);                 // rounds to capacity 4
        CHECK(ring.Capacity() == 4);
        for (int i = 0; i < 4; i++)
            CHECK(ring.Push(MidiEvent::NoteOn(0, (uint8_t)(60 + i), 100)));
        CHECK(!ring.Push(MidiEvent::NoteOn(0, 72, 100)));   // full -> drop
        CHECK(ring.ReadAvailable() == 4);

        MidiEvent buf[8];
        CHECK(ring.Read(buf, 8) == 4);
        CHECK(buf[0].data1 == 60 && buf[3].data1 == 63);    // FIFO order
        CHECK(ring.ReadAvailable() == 0);

        // Drain-and-refill exercises index wrap past capacity.
        for (int i = 0; i < 3; i++) CHECK(ring.Push(MidiEvent::NoteOn(0, 40, 90)));
        CHECK(ring.Read(buf, 2) == 2);
        CHECK(ring.Push(MidiEvent::NoteOn(0, 41, 90)));      // wraps
        CHECK(ring.ReadAvailable() == 2);
    }

    const Frame SR = 48000;   // 1 second = 48000 frames

    // --- MidiRecorder: a simple two-note take --------------------------------
    {
        MidiRecorder rec;
        rec.Begin(/*startFrame=*/SR);          // take starts at 1.0 s
        // C4 held 0.5 s starting at take start.
        rec.OnEvent(MidiEvent::NoteOn(0, 60, 100), SR);
        rec.OnEvent(MidiEvent::NoteOff(0, 60, 0), SR + SR / 2);
        // E4 held from +0.25 s to +0.75 s.
        rec.OnEvent(MidiEvent::NoteOn(0, 64, 80),  SR + SR / 4);
        CHECK(rec.HasOpenNotes());
        rec.OnEvent(MidiEvent::NoteOff(0, 64, 0),  SR + (3 * SR) / 4);
        CHECK(!rec.HasOpenNotes());
        CHECK(rec.ClosedNoteCount() == 2);

        MidiClip clip = rec.End(SR + SR);      // 1.0 s take
        CHECK(clip.startFrame == SR);
        CHECK(clip.lengthFrames == SR);
        CHECK(clip.notes.size() == 2);
        // Notes are clip-relative.
        const MidiNote& n0 = clip.notes[0];
        CHECK(n0.pitch == 60 && n0.velocity == 100);
        CHECK(n0.startFrame == 0 && n0.lengthFrames == SR / 2);
        const MidiNote& n1 = clip.notes[1];
        CHECK(n1.pitch == 64 && n1.velocity == 80);
        CHECK(n1.startFrame == SR / 4 && n1.lengthFrames == SR / 2);
    }

    // --- Held note at take end is closed at endFrame -------------------------
    {
        MidiRecorder rec;
        rec.Begin(0);
        rec.OnEvent(MidiEvent::NoteOn(0, 48, 120), 100);
        // No note-off; End() must close it.
        MidiClip clip = rec.End(5000);
        CHECK(clip.notes.size() == 1);
        CHECK(clip.notes[0].startFrame == 100);
        CHECK(clip.notes[0].lengthFrames == 4900);   // 5000 - 100
    }

    // --- Retrigger: same key pressed again before release splits into two ----
    {
        MidiRecorder rec;
        rec.Begin(0);
        rec.OnEvent(MidiEvent::NoteOn(0, 60, 100), 0);
        rec.OnEvent(MidiEvent::NoteOn(0, 60, 110), 1000);   // retrigger
        rec.OnEvent(MidiEvent::NoteOff(0, 60, 0), 2000);
        MidiClip clip = rec.End(2000);
        CHECK(clip.notes.size() == 2);
        CHECK(clip.notes[0].startFrame == 0    && clip.notes[0].lengthFrames == 1000);
        CHECK(clip.notes[1].startFrame == 1000 && clip.notes[1].velocity == 110);
    }

    // --- Overlapping keys are independent; stray note-off ignored ------------
    {
        MidiRecorder rec;
        rec.Begin(0);
        rec.OnEvent(MidiEvent::NoteOff(0, 72, 0), 10);      // no matching on -> ignored
        rec.OnEvent(MidiEvent::NoteOn(0, 60, 100), 0);
        rec.OnEvent(MidiEvent::NoteOn(0, 67, 100), 500);    // chord: two keys held
        rec.OnEvent(MidiEvent::NoteOff(0, 60, 0), 1000);
        rec.OnEvent(MidiEvent::NoteOff(0, 67, 0), 1500);
        MidiClip clip = rec.End(1500);
        CHECK(clip.notes.size() == 2);
    }

    // --- MidiRecorder::SnapshotNotes: in-progress take (closed + held) -------
    {
        MidiRecorder rec;
        rec.Begin(0);
        rec.OnEvent(MidiEvent::NoteOn(0, 60, 100), 0);
        rec.OnEvent(MidiEvent::NoteOff(0, 60, 0), 1000);   // closed
        rec.OnEvent(MidiEvent::NoteOn(0, 64, 90), 500);    // still held
        std::vector<MidiNote> snap = rec.SnapshotNotes(2000);
        CHECK(snap.size() == 2);
        // Closed C4.
        CHECK(snap[0].pitch == 60 && snap[0].startFrame == 0 && snap[0].lengthFrames == 1000);
        // Held E4 extended to now (2000 - 500).
        CHECK(snap[1].pitch == 64 && snap[1].startFrame == 500 && snap[1].lengthFrames == 1500);
        // Snapshot does not finalize: End still closes the held note.
        MidiClip clip = rec.End(3000);
        CHECK(clip.notes.size() == 2);
        CHECK(clip.notes[1].lengthFrames == 2500);   // 3000 - 500
    }

    // --- SplitMidiLoopTakes: loop-recorded notes -> per-pass takes -----------
    {
        std::vector<MidiNote> rec;
        rec.push_back(MidiNote{60, 100, 100,  200});   // pass 0
        rec.push_back(MidiNote{64, 100, 1200, 200});   // pass 1 (loopLen 1000)
        rec.push_back(MidiNote{67, 100, 2300, 200});   // pass 2
        auto takes = SplitMidiLoopTakes(rec, 1000);
        CHECK(takes.size() == 3);
        CHECK(takes[0].size() == 1 && takes[0][0].startFrame == 100);   // re-based
        CHECK(takes[1].size() == 1 && takes[1][0].startFrame == 200);
        CHECK(takes[2].size() == 1 && takes[2][0].startFrame == 300);
        CHECK(takes[0][0].pitch == 60 && takes[2][0].pitch == 67);
        // A silent middle pass leaves an empty take slot.
        std::vector<MidiNote> gap;
        gap.push_back(MidiNote{60, 100, 50,   100});   // pass 0
        gap.push_back(MidiNote{72, 100, 2050, 100});   // pass 2 (pass 1 empty)
        auto g = SplitMidiLoopTakes(gap, 1000);
        CHECK(g.size() == 3 && g[1].empty());
    }

    // --- Mute groups: muting any member mutes the whole group ----------------
    {
        Project p;
        Track a; a.id = p.NextTrackId(); a.name = "A"; p.AddTrack(a);
        Track b; b.id = p.NextTrackId(); b.name = "B"; p.AddTrack(b);
        Track c; c.id = p.NextTrackId(); c.name = "C"; p.AddTrack(c);
        // A and B in group 1; C ungrouped.
        SetTrackMuteGroupCommand(a.id, 1).Do(p);
        SetTrackMuteGroupCommand(b.id, 1).Do(p);

        SetTrackMuteCommand mute(a.id, true);
        CHECK(mute.Do(p));
        CHECK(p.FindTrack(a.id)->muted && p.FindTrack(b.id)->muted);   // both
        CHECK(!p.FindTrack(c.id)->muted);                              // not C
        mute.Undo(p);
        CHECK(!p.FindTrack(a.id)->muted && !p.FindTrack(b.id)->muted); // restored

        // Ungrouped track mutes only itself.
        SetTrackMuteCommand mc(c.id, true);
        CHECK(mc.Do(p));
        CHECK(p.FindTrack(c.id)->muted);
        CHECK(!p.FindTrack(a.id)->muted && !p.FindTrack(b.id)->muted);

        // Group assignment round-trips through ProjectIO.
        const char* path = "mutegroup_roundtrip.dawproj";
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        std::remove(path);
        CHECK(q.Tracks()[0].muteGroup == 1 && q.Tracks()[1].muteGroup == 1);
        CHECK(q.Tracks()[2].muteGroup == 0);
    }

    // --- SetTrackInputCommand: assign / undo a track's record input ----------
    {
        Project p;
        Track t; t.id = p.NextTrackId(); t.type = TrackType::Midi; t.name = "Keys";
        p.AddTrack(t);
        const TrackId id = t.id;

        InputSource src; src.kind = InputSource::kMidi; src.name = "/dev/midi/usb/0-0";
        SetTrackInputCommand cmd(id, src);
        CHECK(cmd.Do(p));
        CHECK(p.FindTrack(id)->input.kind == InputSource::kMidi);
        CHECK(p.FindTrack(id)->input.name == "/dev/midi/usb/0-0");
        cmd.Undo(p);
        CHECK(p.FindTrack(id)->input.kind == InputSource::kNone);
        CHECK(p.FindTrack(id)->input.name.empty());
    }

    // --- ProjectIO round-trips the input source ------------------------------
    {
        Project p;
        Track t; t.id = p.NextTrackId(); t.type = TrackType::Midi; t.name = "Keys";
        t.input.kind = InputSource::kMidi;
        t.input.name = "/dev/midi/usb/0-0";
        t.input.channel = 2;
        p.AddTrack(t);

        const char* path = "midi_input_roundtrip.dawproj";
        CHECK(ProjectIO::Save(p, path));
        Project q;
        CHECK(ProjectIO::Load(q, path));
        std::remove(path);
        CHECK(q.Tracks().size() == 1);
        const InputSource& in = q.Tracks()[0].input;
        CHECK(in.kind == InputSource::kMidi);
        CHECK(in.name == "/dev/midi/usb/0-0");
        CHECK(in.channel == 2);

        // A track with no input assigned writes no `input` line and loads kNone.
        Project p2;
        Track t2; t2.id = p2.NextTrackId(); t2.type = TrackType::Audio; t2.name = "Aud";
        p2.AddTrack(t2);
        CHECK(ProjectIO::Save(p2, path));
        Project q2;
        CHECK(ProjectIO::Load(q2, path));
        std::remove(path);
        CHECK(q2.Tracks()[0].input.kind == InputSource::kNone);
    }

    std::printf("midi_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
