// Host tests for the kit-free MIDI core: the SPSC event ring and the
// MidiRecorder note-pairing (live events -> model MidiClip).
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/midi_tests.cpp src/midi/MidiRecorder.cpp \
//       src/model/Project.cpp -o /tmp/midi && /tmp/midi
#include "../src/midi/MidiEventRing.h"
#include "../src/midi/MidiRecorder.h"

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

    std::printf("midi_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
