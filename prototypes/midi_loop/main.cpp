// midi_loop — Midi Kit 2 adapter driver. Two modes:
//
//   midi_loop                 loopback self-test: our producer -> our consumer
//                             -> MidiRecorder. Proves the adapter with no
//                             hardware (a keyboard stand-in). Deterministic.
//
//   midi_loop listen [secs]   enumerate endpoints, connect the first external
//                             producer (a keyboard, e.g. /dev/midi/usb/0-0) to
//                             our consumer, and record live notes for `secs`
//                             (default 10) into a MidiClip, printed at the end.
//
// Needs the desktop session (midi_server running) for the roster / hardware
// endpoints. The loopback path works even without midi_server.

#include "../../src/midi/MidiPort.h"
#include "../../src/midi/MidiRecorder.h"

#include <Application.h>
#include <OS.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

using namespace daw;

static const double kSampleRate = 48000.0;

static void PrintClip(const MidiClip& clip) {
    std::printf("\nMidiClip: start=%lld length=%lld frames, %zu notes\n",
                (long long)clip.startFrame, (long long)clip.lengthFrames,
                clip.notes.size());
    for (size_t i = 0; i < clip.notes.size(); i++) {
        const MidiNote& n = clip.notes[i];
        std::printf("  note %2zu: pitch=%3d vel=%3d start=%lld len=%lld\n",
                    i, n.pitch, n.velocity,
                    (long long)n.startFrame, (long long)n.lengthFrames);
    }
}

// Loopback: spray a small phrase through the kit and verify it comes back.
static int RunLoopback() {
    MidiOutputPort out("midi_loop out");
    MidiInputPort  in("midi_loop in");
    if (out.Register() != B_OK || in.Register() != B_OK) {
        std::fprintf(stderr, "midi_loop: endpoint Register failed\n");
        return 1;
    }
    if (out.ConnectTo(in.KitConsumer()) != B_OK) {
        std::fprintf(stderr, "midi_loop: Connect failed\n");
        return 1;
    }

    MidiRecorder rec;
    rec.Begin(0);
    const bigtime_t t0 = system_time();

    struct Ev { uint8_t note, vel; double onS, offS; };
    const Ev phrase[] = {
        { 60, 100, 0.00, 0.20 },   // C4
        { 64,  90, 0.10, 0.30 },   // E4 (overlaps)
        { 67, 110, 0.20, 0.45 },   // G4
    };
    // Spray with kit timestamps; then drain and stamp by arrival frame.
    for (const Ev& e : phrase) {
        out.Send(MidiEvent::NoteOn (0, e.note, e.vel));
        usleep(20000);
    }
    // Send the note-offs after their durations.
    usleep(300000);
    for (const Ev& e : phrase)
        out.Send(MidiEvent::NoteOff(0, e.note, 0));
    usleep(100000);

    // Drain everything the consumer queued, stamping each by wall-clock frame.
    MidiEvent buf[64];
    size_t got;
    while ((got = in.ReadEvents(buf, 64)) > 0) {
        for (size_t i = 0; i < got; i++) {
            Frame f = (Frame)(((double)(buf[i].timeUs - t0)) * 1e-6 * kSampleRate);
            if (f < 0) f = 0;
            rec.OnEvent(buf[i], f);
        }
    }
    MidiClip clip = rec.End((Frame)(0.5 * kSampleRate));

    PrintClip(clip);
    const bool ok = clip.notes.size() == 3;
    std::printf("\nloopback %s (expected 3 notes)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// Listen: connect a hardware keyboard's producer and record live notes.
static int RunListen(double seconds) {
    std::printf("== endpoints (midi_server graph) ==\n");
    auto eps = EnumerateMidiEndpoints();
    if (eps.empty())
        std::printf("  (none — is midi_server running? are you in the desktop?)\n");
    int32 producerId = 0;
    for (const auto& e : eps) {
        std::printf("  id=%d  %-24s  producer=%d consumer=%d\n",
                    (int)e.id, e.name.c_str(), e.isProducer, e.isConsumer);
        // First producer that isn't one of our own out ports.
        if (e.isProducer && producerId == 0 &&
            e.name.find("HaikuDAW") == std::string::npos &&
            e.name.find("midi_loop") == std::string::npos)
            producerId = e.id;
    }
    if (producerId == 0) {
        std::fprintf(stderr, "\nmidi_loop: no external producer (keyboard) found\n");
        return 1;
    }

    MidiInputPort in("HaikuDAW In");
    if (in.Register() != B_OK) {
        std::fprintf(stderr, "midi_loop: consumer Register failed\n");
        return 1;
    }
    if (in.ConnectFrom(producerId) != B_OK) {
        std::fprintf(stderr, "midi_loop: ConnectFrom(%d) failed\n", (int)producerId);
        return 1;
    }
    std::printf("\nConnected producer id=%d. Play for %.0f s ...\n",
                (int)producerId, seconds);

    MidiRecorder rec;
    rec.Begin(0);
    const bigtime_t t0 = system_time();
    const bigtime_t end = t0 + (bigtime_t)(seconds * 1e6);

    MidiEvent buf[64];
    while (system_time() < end) {
        size_t got = in.ReadEvents(buf, 64);
        for (size_t i = 0; i < got; i++) {
            Frame f = (Frame)(((double)(buf[i].timeUs - t0)) * 1e-6 * kSampleRate);
            if (f < 0) f = 0;
            rec.OnEvent(buf[i], f);
            if (buf[i].IsNoteOn())
                std::printf("  on  pitch=%3d vel=%3d\n", buf[i].data1, buf[i].data2);
            else if (buf[i].IsNoteOff())
                std::printf("  off pitch=%3d\n", buf[i].data1);
        }
        usleep(5000);
    }
    MidiClip clip = rec.End((Frame)(seconds * kSampleRate));
    PrintClip(clip);
    std::printf("\nlisten done: recorded %zu notes\n", clip.notes.size());
    return 0;
}

int main(int argc, char** argv) {
    // A BApplication is needed for the Midi Kit roster / midi_server messaging.
    BApplication app("application/x-vnd.DuskAudio-midi_loop");

    if (argc > 1 && std::strcmp(argv[1], "listen") == 0) {
        double secs = (argc > 2) ? std::atof(argv[2]) : 10.0;
        return RunListen(secs);
    }
    return RunLoopback();
}
