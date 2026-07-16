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

// Put an absolute-frame note onto a MIDI track via a single region at frame 0
// (relative == absolute), growing the region window to cover it.
static void PutNote(Track& t, const MidiNote& n) {
    if (t.midiClips.empty()) { MidiClip c; c.id = 1; c.startFrame = 0;
                               c.lengthFrames = 1; t.midiClips.push_back(c); }
    MidiClip& c = t.midiClips.front();
    c.notes.push_back(n);
    const Frame end = n.startFrame + n.lengthFrames;
    if (end > c.lengthFrames) c.lengthFrames = end;
}

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
    PutNote(t, note);

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
        PutNote(m, note);
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

    // --- Aux send: a track routed to master PLUS a post-fader send into a
    // bus (also -> master) at `level`. The bus is center-panned, so equal-power
    // pan attenuates its input by cos(45)=0.7071; the summed peak is therefore
    // (1 + level*0.7071)x the direct-only peak. Validates that the send edge
    // orders the bus after the sender (send actually lands in the mix).
    auto peakWithSend = [&](float level) -> float {
        Project pr;
        pr.sampleRate = SR;
        pr.masterGain = 1.0f;
        Track b;
        b.id = pr.NextTrackId(); b.type = TrackType::Bus;
        b.gain = 1.0f; b.pan = 0.0f;         // unity aux bus -> master
        Track m;
        m.id = pr.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f;
        PutNote(m, note);
        m.output = kInvalidTrackId;          // direct to master
        if (level > 0.0f) m.sends.push_back(Send{b.id, level, false});
        pr.AddTrack(b);
        pr.AddTrack(m);

        const std::string p = "/tmp/haiku_daw_export_send.wav";
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
    const float pDirect = peakWithSend(0.0f);   // no send: direct only
    const float pSend   = peakWithSend(1.0f);   // + unity send through the bus
    const float kBusPan = 0.70710678f;          // center equal-power gain
    CHECK(pDirect > 0.01f);
    CHECK(std::fabs(pSend - pDirect * (1.0f + kBusPan)) < pDirect * 0.05f);

    // Regression: a self-send (dest == own track) must be ignored, not collapse
    // the routing graph to flat. Export a MIDI track with a self-send and check
    // the peak matches the plain direct case (self-send neither doubles nor
    // breaks routing).
    {
        Project pr;
        pr.sampleRate = SR;
        Track m;
        m.id = pr.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f; PutNote(m, note);
        m.sends.push_back(Send{ m.id, 1.0f, false });   // self-send
        pr.AddTrack(m);
        const std::string p = "/tmp/haiku_daw_export_selfsend.wav";
        std::remove(p.c_str());
        CHECK(ExportWav(pr, p, SR));
        WavSource s; CHECK(s.Open(p));
        float pk = 0.0f; const float* c = nullptr; size_t f = 0;
        while (s.ReadChunk(&c, &f))
            for (size_t i = 0; i < f * 2; ++i) pk = std::max(pk, std::fabs(c[i]));
        std::remove(p.c_str());
        CHECK(std::fabs(pk - pDirect) < pDirect * 0.05f);   // == plain direct
    }

    // --- Gain automation: a full-scale note with a 1.0 -> 0.0 gain ramp over
    // the buffer should leave the second half much quieter than the first.
    {
        Project pr;
        pr.sampleRate = SR;
        pr.masterGain = 1.0f;
        Track m;
        m.id = pr.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f;
        MidiNote n2 = note;
        n2.startFrame = 0; n2.lengthFrames = (Frame)SR;   // 1 s sustained
        PutNote(m, n2);
        m.gainAuto.AddPoint(0, 1.0f);
        m.gainAuto.AddPoint((Frame)SR, 0.0f);             // ramp to silence
        pr.AddTrack(m);

        const std::string p = "/tmp/haiku_daw_export_auto.wav";
        std::remove(p.c_str());
        CHECK(ExportWav(pr, p, SR));
        WavSource s;
        CHECK(s.Open(p));
        std::vector<float> all;
        const float* c = nullptr; size_t f = 0;
        while (s.ReadChunk(&c, &f))
            for (size_t i = 0; i < f * 2; ++i) all.push_back(c[i]);
        std::remove(p.c_str());
        const size_t half = all.size() / 2;
        float pEarly = 0.0f, pLate = 0.0f;
        for (size_t i = 0; i < half; ++i)          pEarly = std::max(pEarly, std::fabs(all[i]));
        for (size_t i = half; i < all.size(); ++i) pLate  = std::max(pLate,  std::fabs(all[i]));
        CHECK(pEarly > 0.05f);
        CHECK(pLate < pEarly * 0.65f);             // ramp made the tail quiet
    }

    // Stems: two MIDI tracks -> two isolated WAV files, each with signal.
    {
        Project p; p.sampleRate = SR; p.masterGain = 1.0f;
        for (int k = 0; k < 2; k++) {
            Track tr; tr.id = p.NextTrackId(); tr.type = TrackType::Midi;
            tr.name = k == 0 ? "Kick" : "Snare";
            MidiNote n; n.pitch = 60 + k * 4; n.velocity = 110;
            n.startFrame = 0; n.lengthFrames = (Frame)(SR / 2);
            PutNote(tr, n);
            p.AddTrack(tr);
        }
        const int wrote = ExportStems(p, ".", SR);
        CHECK(wrote == 2);
        for (const char* f : { "./01_Kick.wav", "./02_Snare.wav" }) {
            WavSource s;
            CHECK(s.Open(f));
            if (s.IsValid()) CHECK(s.TotalFrames() > 0);
            std::remove(f);
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
