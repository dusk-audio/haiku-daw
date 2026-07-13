// Host-buildable tests for the offline WAV exporter. Builds a tiny project
// (one MIDI track with a single note, so no external audio file is needed),
// bounces it to a temp WAV, reads the file back with WavSource, and asserts
// the file is valid, non-empty, and actually contains rendered signal.

#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/model/Project.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

int main() {
    const double SR = 48000.0;

    // Build a project: one MIDI track, one half-second note (A4).
    Project project;
    project.sampleRate = SR;
    project.masterGain = 1.0f;

    Track t;
    t.id   = project.NextTrackId();
    t.type = TrackType::Midi;
    t.name = "synth";
    t.gain = 1.0f;
    t.pan  = 0.0f;

    MidiNote note;
    note.pitch        = 69;               // A4
    note.velocity     = 110;
    note.startFrame   = 0;
    note.lengthFrames = static_cast<Frame>(SR / 2);   // 0.5 s
    t.notes.push_back(note);

    CHECK(project.AddTrack(t));

    // Export to a temp file.
    const std::string path = "/tmp/haiku_daw_export_test.wav";
    std::remove(path.c_str());
    const bool ok = ExportWav(project, path, SR);
    CHECK(ok);

    // Read it back.
    WavSource src;
    CHECK(src.Open(path));
    CHECK(src.IsValid());
    CHECK(src.TotalFrames() > 0);
    // Note is 0.5 s; expect roughly that many frames (allow slack).
    CHECK(src.TotalFrames() >= static_cast<int64_t>(SR / 2) - 4);

    // Scan for any non-zero sample: the note must have rendered.
    float peak = 0.0f;
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames)) {
        for (size_t i = 0; i < frames * 2; ++i) {
            const float a = std::fabs(chunk[i]);
            if (a > peak) peak = a;
        }
    }
    CHECK(peak > 0.01f);

    std::remove(path.c_str());

    // --- Bus routing: the same note through a bus, at bus gain 1.0 vs 0.5,
    // should scale the exported peak by ~0.5 (validates the topo bus mix).
    auto peakThroughBus = [&](float busGain) -> float {
        Project pr;
        pr.sampleRate = SR;
        pr.masterGain = 1.0f;
        Track m;
        m.id = pr.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f;
        m.notes.push_back(note);
        Track b;
        b.id = pr.NextTrackId(); b.type = TrackType::Bus;
        b.gain = busGain; b.pan = 0.0f;
        m.output = b.id;                 // route the synth into the bus
        pr.AddTrack(m);
        pr.AddTrack(b);

        const std::string p = "/tmp/haiku_daw_export_bus.wav";
        std::remove(p.c_str());
        if (!ExportWav(pr, p, SR)) return -1.0f;
        WavSource s;
        if (!s.Open(p)) return -1.0f;
        float pk = 0.0f; const float* c = nullptr; size_t f = 0;
        while (s.ReadChunk(&c, &f))
            for (size_t i = 0; i < f * 2; ++i) { float a = std::fabs(c[i]); if (a > pk) pk = a; }
        std::remove(p.c_str());
        return pk;
    };
    const float pFull = peakThroughBus(1.0f);
    const float pHalf = peakThroughBus(0.5f);
    CHECK(pFull > 0.01f);
    CHECK(pHalf > 0.005f);
    CHECK(std::fabs(pHalf - pFull * 0.5f) < pFull * 0.1f);   // bus fader halves it

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
