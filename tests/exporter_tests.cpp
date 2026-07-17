// Host-buildable tests for the offline WAV exporter. Builds a tiny project
// (one MIDI track with a single note, so no external audio file is needed),
// bounces it to a temp WAV, reads the file back with WavSource, and asserts
// the file is valid, non-empty, and actually contains rendered signal.

#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/dsp/Loudness.h"
#include "../src/model/Project.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
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

    // --- CC7 (channel volume) scales the synth: 0 mutes, 64 ~= half, 127 full.
    auto peakWithCC7 = [&](int vol) -> float {
        Project pr; pr.sampleRate = SR; pr.masterGain = 1.0f;
        Track m; m.id = pr.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f;
        MidiNote n2 = note; n2.startFrame = 0; n2.lengthFrames = (Frame)(SR / 2);
        PutNote(m, n2);
        m.midiClips.front().events.push_back({ MidiClipEvent::CC, 0, 7, vol });
        pr.AddTrack(m);
        const std::string p = "/tmp/haiku_daw_export_cc7.wav";
        std::remove(p.c_str());
        if (!ExportWav(pr, p, SR, 32)) return -1.0f;   // 32-bit float: exact
        WavSource s; if (!s.Open(p)) return -1.0f;
        float pk = 0.0f; const float* c = nullptr; size_t f = 0;
        while (s.ReadChunk(&c, &f))
            for (size_t i = 0; i < f * 2; ++i) pk = std::max(pk, std::fabs(c[i]));
        std::remove(p.c_str());
        return pk;
    };
    const float pcFull = peakWithCC7(127);
    const float pcMute = peakWithCC7(0);
    const float pcHalf = peakWithCC7(64);
    CHECK(pcFull > 0.05f);
    CHECK(pcMute < pcFull * 0.02f);                              // CC7=0 mutes
    CHECK(std::fabs(pcHalf - pcFull * (64.0f / 127.0f)) < pcFull * 0.1f); // ~half

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

    // Solo-safe: a track stays audible even when another track is soloed.
    {
        auto energy = [](const Project& pr) -> double {
            const char* f = "solosafe_tmp.wav";
            if (!ExportWav(pr, f, 48000.0)) return -1.0;
            WavSource s;
            if (!s.Open(f)) { std::remove(f); return -1.0; }
            double e = 0; const float* c = nullptr; size_t n = 0;
            while (s.ReadChunk(&c, &n))
                for (size_t i = 0; i < n * 2; i++) e += std::fabs(c[i]);
            std::remove(f);
            return e;
        };
        Project p; p.sampleRate = SR;
        Track a; a.id = p.NextTrackId(); a.type = TrackType::Midi; a.soloed = true;
        MidiNote na; na.pitch = 60; na.velocity = 110;
        na.startFrame = 0; na.lengthFrames = (Frame)(SR / 2); PutNote(a, na);
        Track b; b.id = p.NextTrackId(); b.type = TrackType::Midi;
        MidiNote nb; nb.pitch = 67; nb.velocity = 110;
        nb.startFrame = 0; nb.lengthFrames = (Frame)(SR / 2); PutNote(b, nb);
        p.AddTrack(a); p.AddTrack(b);
        const double eSolo = energy(p);            // A soloed -> only A
        Project p2 = p;                            // B solo-safe -> A + B
        for (Track& t : p2.Tracks()) if (!t.soloed) t.soloSafe = true;
        const double eSafe = energy(p2);
        CHECK(eSolo > 0.0);
        CHECK(eSafe > eSolo * 1.2);                // B adds energy despite A's solo
    }

    // Leaf-scratch reuse (OOM fix): several source tracks all feeding ONE bus
    // must each contribute — the shared leaf scratch buffer is reused per track
    // and summed into the bus, so no track's signal is dropped or overwritten.
    {
        auto energyNTracksToBus = [&](int n) -> double {
            Project pr; pr.sampleRate = SR; pr.masterGain = 1.0f;
            Track b; b.id = pr.NextTrackId(); b.type = TrackType::Bus;
            b.gain = 1.0f; b.pan = 0.0f;
            const TrackId bid = b.id;
            pr.AddTrack(b);
            for (int k = 0; k < n; k++) {
                Track m; m.id = pr.NextTrackId(); m.type = TrackType::Midi;
                m.gain = 1.0f; m.pan = 0.0f; m.output = bid;   // leaf -> bus
                MidiNote nk; nk.pitch = 48 + k * 3; nk.velocity = 100;
                nk.startFrame = 0; nk.lengthFrames = (Frame)(SR / 2);
                PutNote(m, nk);
                pr.AddTrack(m);
            }
            const char* p = "leafbus_tmp.wav";
            std::remove(p);
            if (!ExportWav(pr, p, SR)) return -1.0;
            WavSource s; if (!s.Open(p)) { std::remove(p); return -1.0; }
            double e = 0; const float* c = nullptr; size_t f = 0;
            while (s.ReadChunk(&c, &f))
                for (size_t i = 0; i < f * 2; i++) e += std::fabs(c[i]);
            std::remove(p);
            return e;
        };
        const double e1 = energyNTracksToBus(1);
        const double e4 = energyNTracksToBus(4);
        CHECK(e1 > 0.0);
        CHECK(e4 > e1 * 1.5);   // 4 tracks land (a scratch clobber would give ~1x)
    }

    // Bus-routed stem must NOT be silent: a soloed source feeding a (non-soloed)
    // bus must still pass through the bus to master. Regression for the stem-
    // silence bug where a solo-excluded bus was skipped entirely.
    {
        Project p; p.sampleRate = SR; p.masterGain = 1.0f;
        Track bus; bus.id = p.NextTrackId(); bus.type = TrackType::Bus;
        bus.name = "Drums"; bus.gain = 1.0f; bus.pan = 0.0f;
        Track src; src.id = p.NextTrackId(); src.type = TrackType::Midi;
        src.name = "Kick"; src.gain = 1.0f; src.pan = 0.0f;
        src.output = bus.id;                         // route source INTO the bus
        MidiNote n; n.pitch = 48; n.velocity = 110;
        n.startFrame = 0; n.lengthFrames = (Frame)(SR / 2); PutNote(src, n);
        p.AddTrack(bus); p.AddTrack(src);

        const int wrote = ExportStems(p, ".", SR);
        CHECK(wrote == 1);                           // one source stem (bus skipped)
        WavSource s; CHECK(s.Open("./02_Kick.wav"));
        float pk = 0.0f; const float* c = nullptr; size_t f = 0;
        while (s.ReadChunk(&c, &f))
            for (size_t i = 0; i < f * 2; i++) pk = std::max(pk, std::fabs(c[i]));
        std::remove("./02_Kick.wav");
        CHECK(pk > 0.01f);                           // bus-routed stem has signal
    }

    // Solo-safe must NOT bleed into a stem: ExportStems isolates one track even
    // when another is soloSafe.
    {
        Project p; p.sampleRate = SR; p.masterGain = 1.0f;
        Track a; a.id = p.NextTrackId(); a.type = TrackType::Midi; a.name = "A";
        MidiNote na; na.pitch = 60; na.velocity = 110;
        na.startFrame = 0; na.lengthFrames = (Frame)(SR / 2); PutNote(a, na);
        Track b; b.id = p.NextTrackId(); b.type = TrackType::Midi; b.name = "B";
        b.soloSafe = true;                           // would bleed if not cleared
        MidiNote nb2; nb2.pitch = 67; nb2.velocity = 110;
        nb2.startFrame = 0; nb2.lengthFrames = (Frame)(SR / 2); PutNote(b, nb2);
        p.AddTrack(a); p.AddTrack(b);

        ExportStems(p, ".", SR);
        auto energy = [&](const char* f) -> double {
            WavSource s; if (!s.Open(f)) return -1.0;
            double e = 0; const float* c = nullptr; size_t n = 0;
            while (s.ReadChunk(&c, &n))
                for (size_t i = 0; i < n * 2; i++) e += std::fabs(c[i]);
            std::remove(f);
            return e;
        };
        const double ea = energy("./01_A.wav");
        const double eb = energy("./02_B.wav");
        // A's stem should be ~one track's worth, not A+B (no soloSafe bleed).
        CHECK(ea > 0.0 && eb > 0.0);
        CHECK(std::fabs(ea - eb) < eb * 0.5);        // comparable, neither doubled
    }

    // Bit-depth export: 24-bit PCM and 32-bit float bounces must be valid,
    // readable by WavSource, carry the rendered signal, AND be encoded in the
    // requested format (fmt chunk audioFormat + bitsPerSample match the depth).
    {
        // Read the fmt chunk's audioFormat (offset 20) and bitsPerSample (34)
        // from a canonical 44-byte-header WAV as written by WavWriter.
        auto fmtOf = [](const std::string& path, uint16_t* fmt, uint16_t* bits) {
            std::ifstream f(path, std::ios::binary);
            uint8_t h[36];
            f.read((char*)h, 36);
            if (f.gcount() != 36) { *fmt = 0; *bits = 0; return; }
            *fmt  = (uint16_t)(h[20] | (h[21] << 8));
            *bits = (uint16_t)(h[34] | (h[35] << 8));
        };
        Project p; p.sampleRate = SR; p.masterGain = 1.0f;
        Track m; m.id = p.NextTrackId(); m.type = TrackType::Midi;
        m.gain = 1.0f; m.pan = 0.0f; PutNote(m, note);
        p.AddTrack(m);
        for (int depth : { 24, 32 }) {
            const std::string path = "/tmp/haiku_daw_depth.wav";
            std::remove(path.c_str());
            CHECK(ExportWav(p, path, SR, depth));
            uint16_t fmt = 0, bits = 0;
            fmtOf(path, &fmt, &bits);
            CHECK(bits == depth);                          // encoded at requested depth
            CHECK(fmt == (depth == 32 ? 3 : 1));           // 32 = IEEE float, else PCM
            WavSource s;
            CHECK(s.Open(path));
            CHECK(s.IsValid());
            CHECK(s.TotalFrames() > 0);
            float peak = 0.0f; const float* c = nullptr; size_t f = 0;
            while (s.ReadChunk(&c, &f))
                for (size_t i = 0; i < f * 2; i++) peak = std::max(peak, std::fabs(c[i]));
            CHECK(peak > 0.01f);                    // signal survived quantization
            std::remove(path.c_str());
        }
    }

    // NaN-injection: a float-WAV source carrying NaN/Inf samples must NOT reach
    // the exported master. The final finite sweep zeroes them (parity with the
    // RT engine's pre-DAC guard); without it the whole bounce is poisoned.
    {
        // Write a 32-bit float stereo WAV whose samples include NaN/Inf.
        const char* nanWav = "/tmp/haiku_daw_naninject.wav";
        auto put_u32 = [](std::ofstream& f, uint32_t v) {
            uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) };
            f.write((char*)b, 4);
        };
        auto put_u16 = [](std::ofstream& f, uint16_t v) {
            uint8_t b[2] = { uint8_t(v), uint8_t(v>>8) }; f.write((char*)b, 2);
        };
        const int frames = 4800;                     // 0.1 s @ 48k
        std::vector<float> s(frames * 2);
        const float qnan = std::numeric_limits<float>::quiet_NaN();
        const float inf  = std::numeric_limits<float>::infinity();
        for (int i = 0; i < frames; i++) {
            s[i * 2 + 0] = (i % 3 == 0) ? qnan : (i % 3 == 1 ? inf : 0.3f);
            s[i * 2 + 1] = (i % 5 == 0) ? -inf : 0.2f;
        }
        {
            std::ofstream f(nanWav, std::ios::binary);
            const uint32_t dataBytes = (uint32_t)(s.size() * sizeof(float));
            f.write("RIFF", 4); put_u32(f, 36 + dataBytes); f.write("WAVE", 4);
            f.write("fmt ", 4); put_u32(f, 16);
            put_u16(f, 3);              // IEEE float
            put_u16(f, 2);             // stereo
            put_u32(f, 48000);
            put_u32(f, 48000 * 2 * 4); // byte rate
            put_u16(f, 2 * 4);         // block align
            put_u16(f, 32);            // bits
            f.write("data", 4); put_u32(f, dataBytes);
            f.write((const char*)s.data(), dataBytes);
        }

        Project p; p.sampleRate = SR; p.masterGain = 1.0f;
        Track t; t.id = p.NextTrackId(); t.type = TrackType::Audio;
        t.gain = 1.0f; t.pan = 0.0f;
        Clip c; c.startFrame = 0; c.lengthFrames = frames;
        c.sourceOffset = 0; c.sourcePath = nanWav; c.gain = 1.0f;
        t.clips.push_back(c);
        p.AddTrack(t);

        const std::string out = "/tmp/haiku_daw_nanout.wav";
        std::remove(out.c_str());
        // Export 32-bit FLOAT: WriteFloat stores those verbatim (no quantizer to
        // clamp NaN/Inf), so the only thing that can keep them out of the file is
        // the Exporter's own finite sweep. A 16-bit bounce would mask it (the
        // PCM quantizer sanitizes too), so this isolates the export guard.
        CHECK(ExportWav(p, out, SR, 32));
        WavSource s2; CHECK(s2.Open(out));
        bool allFinite = true; float pk = 0.0f;
        const float* c2 = nullptr; size_t f2 = 0;
        while (s2.ReadChunk(&c2, &f2))
            for (size_t i = 0; i < f2 * 2; i++) {
                if (!std::isfinite(c2[i])) allFinite = false;
                pk = std::max(pk, std::fabs(c2[i]));
            }
        CHECK(allFinite);              // guard zeroed every NaN/Inf
        CHECK(std::isfinite(pk));
        CHECK(pk <= 1.0f + 1e-3f);     // no full-scale Inf blast survived
        std::remove(out.c_str());
        std::remove(nanWav);
    }

    // Loudness normalization: export a sustained note normalized to a target
    // integrated LUFS with a true-peak ceiling. Measure the output's integrated
    // loudness + true peak and confirm (a) it hit the target, and (b) a loud
    // target that would clip is backed off to respect the ceiling.
    {
        // Measure integrated LUFS + true-peak dBTP of a 32-bit-float WAV.
        auto measure = [](const std::string& path, float* lufs, float* tp) -> bool {
            WavSource s;
            if (!s.Open(path)) return false;
            Loudness m; m.Prepare(s.FrameRate()); m.SetIntegratedEnabled(true);
            const float* c = nullptr; size_t f = 0;
            while (s.ReadChunk(&c, &f)) m.Process(c, (int)f);
            *lufs = m.IntegratedLufs();
            *tp   = m.TruePeakDb();
            return true;
        };
        auto build = []() {
            Project p; p.sampleRate = 48000.0; p.masterGain = 1.0f;
            Track m; m.id = p.NextTrackId(); m.type = TrackType::Midi;
            m.gain = 0.5f; m.pan = 0.0f;      // start well below the target
            MidiNote n; n.pitch = 57; n.velocity = 100;
            n.startFrame = 0; n.lengthFrames = (Frame)(48000 * 3);   // 3 s > gate
            if (m.midiClips.empty()) { MidiClip mc; mc.id = 1; mc.startFrame = 0;
                                       mc.lengthFrames = 1; m.midiClips.push_back(mc); }
            m.midiClips.front().notes.push_back(n);
            m.midiClips.front().lengthFrames = n.lengthFrames;
            p.AddTrack(m);
            return p;
        };
        const std::string path = "/tmp/haiku_daw_norm.wav";

        // (a) Normalize to -16 LUFS, ceiling -1 dBTP. Sine-ish note has low crest,
        // so the target is reachable without hitting the ceiling.
        {
            Project p = build();
            ExportNormalize nz; nz.enabled = true;
            nz.targetLufs = -16.0f; nz.truePeakCeil = -1.0f;
            std::remove(path.c_str());
            CHECK(ExportWav(p, path, 48000.0, 32, nz));
            float lufs = 0, tp = 0;
            CHECK(measure(path, &lufs, &tp));
            CHECK(std::fabs(lufs - (-16.0f)) < 1.5f);   // hit the loudness target
            CHECK(tp <= -1.0f + 0.5f);                  // within the ceiling
            std::remove(path.c_str());
        }

        // (b) Ask for a very loud target (-3 LUFS): the true-peak ceiling must
        // win, so the output true peak stays at/under the ceiling (no clipping).
        {
            Project p = build();
            ExportNormalize nz; nz.enabled = true;
            nz.targetLufs = -3.0f; nz.truePeakCeil = -1.0f;
            std::remove(path.c_str());
            CHECK(ExportWav(p, path, 48000.0, 32, nz));
            float lufs = 0, tp = 0;
            CHECK(measure(path, &lufs, &tp));
            CHECK(tp <= -1.0f + 0.3f);                  // ceiling respected
            std::remove(path.c_str());
        }

        // (c) Same loud target (-3 LUFS) WITH the look-ahead limiter: instead of
        // backing the whole program off, the limiter holds the ceiling — so the
        // output gets much closer to the target loudness than (b) did, while the
        // true peak still stays at/under the ceiling.
        {
            Project p = build();
            ExportNormalize nz; nz.enabled = true; nz.limiter = true;
            nz.targetLufs = -3.0f; nz.truePeakCeil = -1.0f;
            std::remove(path.c_str());
            CHECK(ExportWav(p, path, 48000.0, 32, nz));
            float lufs = 0, tp = 0;
            CHECK(measure(path, &lufs, &tp));
            CHECK(tp <= -1.0f + 0.3f);                  // ceiling still respected
            CHECK(lufs > -8.0f);                        // reached loud (backoff couldn't)
            std::remove(path.c_str());
        }

        // (d) Limiter without normalization: a hot master (source gain 1.0 into
        // a full-velocity note) is peak-limited to the ceiling with no loudness
        // change requested.
        {
            Project p = build();
            for (Track& t : p.Tracks()) t.gain = 1.0f;   // hotter than build()'s 0.5
            ExportNormalize nz; nz.enabled = false; nz.limiter = true;
            nz.truePeakCeil = -1.0f;
            std::remove(path.c_str());
            CHECK(ExportWav(p, path, 48000.0, 32, nz));
            float lufs = 0, tp = 0;
            CHECK(measure(path, &lufs, &tp));
            CHECK(tp <= -1.0f + 0.3f);                  // limiter held the ceiling
            std::remove(path.c_str());
        }
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
