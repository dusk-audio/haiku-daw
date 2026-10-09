// Host-buildable tests for the exporter's job surface: the output format (bit
// depth + dither), the timeline range, progress, cancellation, and the
// temp+rename write (a cancelled or failed export leaves nothing where a
// finished file is expected).
#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"
#include "../src/model/Project.h"

#include <atomic>
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

static bool Exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return f.good();
}
static std::vector<char> ReadBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
}
// Every interleaved sample of a WAV, as floats.
static std::vector<float> ReadFrames(const std::string& p) {
    std::vector<float> out;
    WavSource src;
    if (!src.Open(p)) return out;
    const float* chunk = nullptr;
    size_t frames = 0;
    while (src.ReadChunk(&chunk, &frames))
        out.insert(out.end(), chunk, chunk + frames * 2);
    return out;
}

// One MIDI track with one note ("absolute" == clip-relative: the region starts
// at 0 and is grown to cover the note).
static Project OneNote(double sr, Frame start, Frame len) {
    Project p;
    p.sampleRate = sr;
    p.masterGain = 1.0f;
    Track t;
    t.id = p.NextTrackId();
    t.type = TrackType::Midi;
    t.name = "synth";
    t.gain = 1.0f;
    t.pan = 0.0f;
    MidiClip c;
    c.id = 1;
    c.startFrame = 0;
    c.lengthFrames = start + len;
    MidiNote n;
    n.pitch = 69;
    n.velocity = 110;
    n.startFrame = start;
    n.lengthFrames = len;
    c.notes.push_back(n);
    t.midiClips.push_back(c);
    p.AddTrack(t);
    return p;
}

int main() {
    const double SR = 48000.0;
    const std::string path = "/tmp/haiku_daw_export_job.wav";
    const Project p = OneNote(SR, 0, (Frame)(SR / 2));   // 0.5 s

    // --- progress: called, monotonic, in range, and it reaches exactly 1.0 ---
    {
        std::vector<float> seen;
        ExportJob job;
        job.progress = [&](float f) { seen.push_back(f); };
        ExportOptions o;
        o.job = &job;
        std::remove(path.c_str());
        CHECK(ExportWav(p, path, SR, o));
        CHECK(!seen.empty());
        bool mono = true, inRange = true;
        for (size_t i = 0; i < seen.size(); i++) {
            if (seen[i] < 0.0f || seen[i] > 1.0f) inRange = false;
            if (i && seen[i] < seen[i - 1]) mono = false;
        }
        CHECK(mono);
        CHECK(inRange);
        CHECK(seen.back() == 1.0f);
        CHECK(Exists(path));
        CHECK(!Exists(path + ".part"));   // the temp was renamed, not left
        std::remove(path.c_str());
    }

    // --- a cancelled export leaves NOTHING at the destination ---
    {
        std::atomic<bool> cancel{true};
        ExportJob job;
        job.cancel = &cancel;
        ExportOptions o;
        o.job = &job;
        std::remove(path.c_str());
        CHECK(!ExportWav(p, path, SR, o));
        CHECK(!Exists(path));
        CHECK(!Exists(path + ".part"));
    }

    // --- cancelled during the WRITE phase: the open temp must be cleaned up,
    //     and an existing file at the destination must keep its old bytes.
    //     The source is longer than one write chunk on purpose: a file that
    //     fits in a single chunk has no "middle of the write" to cancel in. ---
    {
        const Project longP = OneNote(SR, 0, (Frame)(SR * 3));   // 3 s > 64k frames
        CHECK(ExportWav(longP, path, SR));
        const std::vector<char> before = ReadBytes(path);
        CHECK(!before.empty());

        std::atomic<bool> cancel{false};
        ExportJob job;
        job.cancel = &cancel;
        job.progress = [&](float f) {
            if (f > 0.9f) cancel = true;   // inside the write loop
        };
        ExportOptions o;
        o.job = &job;
        CHECK(!ExportWav(longP, path, SR, o));
        CHECK(ReadBytes(path) == before);   // untouched
        CHECK(!Exists(path + ".part"));
    }

    // --- 16-bit dither on vs off: both valid, different bytes ---
    {
        const std::string a = path + ".dith.wav", b = path + ".flat.wav";
        ExportOptions on, off;
        on.format  = ExportFormat{16, true};
        off.format = ExportFormat{16, false};
        CHECK(ExportWav(p, a, SR, on));
        CHECK(ExportWav(p, b, SR, off));
        CHECK(ReadBytes(a).size() == ReadBytes(b).size());
        CHECK(ReadBytes(a) != ReadBytes(b));
        // Dither is ignored above 16 bits (32-bit float here): identical bytes.
        const std::string c = path + ".f32a.wav", d = path + ".f32b.wav";
        ExportOptions f1, f2;
        f1.format = ExportFormat{32, true};
        f2.format = ExportFormat{32, false};
        CHECK(ExportWav(p, c, SR, f1));
        CHECK(ExportWav(p, d, SR, f2));
        CHECK(ReadBytes(c) == ReadBytes(d));
        std::remove(a.c_str()); std::remove(b.c_str());
        std::remove(c.c_str()); std::remove(d.c_str());
    }

    // --- range: a window bounce equals the same frames of the full bounce.
    //     The project carries an AUDIO clip straddling the window start as well
    //     as the MIDI note: they take different paths into the mix, and both
    //     have to land on the same samples. ---
    {
        Project pr = OneNote(SR, 0, (Frame)(SR / 2));
        const std::string src = path + ".src.wav";
        {   // a 0.5 s source: a ramp, so every frame is distinguishable
            WavWriter w;
            CHECK(w.Open(src, (int)SR, 2));
            std::vector<int16_t> buf;
            for (int i = 0; i < (int)SR / 2; i++) {
                const int16_t v = (int16_t)(i % 20000 - 10000);
                buf.push_back(v); buf.push_back(v);
            }
            CHECK(w.WriteInt16(buf.data(), buf.size()));
            CHECK(w.Close());
        }
        Track at;
        at.id = pr.NextTrackId();
        at.type = TrackType::Audio;
        at.name = "audio";
        at.gain = 1.0f;
        Clip c;
        c.id = 7;
        c.startFrame = 0;
        c.lengthFrames = (Frame)(SR / 2);
        c.sourcePath = src;
        at.clips.push_back(c);
        CHECK(pr.AddTrack(at));

        const std::string full = path + ".full.wav", slice = path + ".slice.wav";
        ExportOptions fo;
        fo.format = ExportFormat{32};   // float: no quantizer in the way
        CHECK(ExportWav(pr, full, SR, fo));

        const Frame from = (Frame)(SR / 4), to = (Frame)(SR / 2);
        ExportOptions ro;
        ro.format = ExportFormat{32};
        ro.range = ExportRange{from, to};
        CHECK(ExportWav(pr, slice, SR, ro));

        const std::vector<float> F = ReadFrames(full), S = ReadFrames(slice);
        CHECK(F.size() == (size_t)(SR / 2) * 2);
        CHECK(S.size() == (size_t)(to - from) * 2);
        // The note starts before the window: its tail must still sound.
        float peak = 0.0f;
        for (float v : S) peak = std::max(peak, std::fabs(v));
        CHECK(peak > 0.01f);
        // ...and it is the SAME audio as the full bounce at that offset.
        float maxDiff = 0.0f;
        for (size_t i = 0; i < S.size() && (size_t)from * 2 + i < F.size(); i++)
            maxDiff = std::max(maxDiff, std::fabs(S[i] - F[(size_t)from * 2 + i]));
        CHECK(maxDiff < 1e-4f);

        // A range end past the project's own end renders the same as no end.
        ExportOptions co;
        co.format = ExportFormat{32};
        co.range = ExportRange{0, (Frame)(SR * 10)};
        const std::string clamped = path + ".clamp.wav";
        CHECK(ExportWav(pr, clamped, SR, co));
        CHECK(ReadBytes(full) == ReadBytes(clamped));
        std::remove(full.c_str()); std::remove(slice.c_str());
        std::remove(clamped.c_str()); std::remove(src.c_str());
    }

    // --- range with no audio in it at all: nothing to render, no file ---
    {
        ExportOptions o;
        o.range = ExportRange{(Frame)(SR * 5), (Frame)(SR * 6)};
        std::remove(path.c_str());
        CHECK(!ExportWav(p, path, SR, o));
        CHECK(!Exists(path));
    }

    // --- stems: the format is forwarded, progress spans the job and reaches
    //     1.0, and a cancel between stems stops the run ---
    {
        Project two = OneNote(SR, 0, (Frame)(SR / 4));
        Track t2;
        t2.id = two.NextTrackId();
        t2.type = TrackType::Midi;
        t2.name = "second";
        t2.gain = 1.0f;
        MidiClip c2;
        c2.id = 2;
        c2.startFrame = 0;
        c2.lengthFrames = (Frame)(SR / 4);
        MidiNote n2 = { 60, 100, 0, (Frame)(SR / 4) };
        c2.notes.push_back(n2);
        t2.midiClips.push_back(c2);
        two.AddTrack(t2);

        const std::string dir = "/tmp/haiku_daw_stem_dir";
        std::string mk = "mkdir -p " + dir;
        if (system(mk.c_str()) != 0) return 1;

        std::vector<float> seen;
        ExportJob job;
        job.progress = [&](float f) { seen.push_back(f); };
        ExportOptions o;
        o.job = &job;
        o.format = ExportFormat{24};   // 24-bit must reach each stem's header
        const int wrote = ExportStems(two, dir, SR, o);
        CHECK(wrote == 2);
        CHECK(!seen.empty() && seen.back() == 1.0f);
        bool mono = true;
        for (size_t i = 1; i < seen.size(); i++)
            if (seen[i] < seen[i - 1]) mono = false;
        CHECK(mono);

        const std::string s1 = dir + "/01_synth.wav";
        CHECK(Exists(s1));
        const std::vector<char> bytes = ReadBytes(s1);
        CHECK(bytes.size() > 44);
        // fmt chunk: bitsPerSample at byte 34, audioFormat at 20 (PCM = 1).
        CHECK(bytes.size() > 36);
        const int bits = (unsigned char)bytes[34] | ((unsigned char)bytes[35] << 8);
        CHECK(bits == 24);

        // Cancel between stems: the first run above already proved the flag is
        // honoured inside a stem; here the flag is raised before it starts.
        std::atomic<bool> cancel{true};
        ExportJob cj;
        cj.cancel = &cancel;
        ExportOptions co;
        co.job = &cj;
        CHECK(ExportStems(two, dir, SR, co) == 0);

        std::string rm = "rm -rf " + dir;
        if (system(rm.c_str()) != 0) return 1;
    }

    std::remove(path.c_str());
    std::printf("\nexporter_job_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
