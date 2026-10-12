// Host-buildable tests for the stretch stage on the exporter's clip path
// (M7.2).
//
// The fixture is a source file built so that WHICH part of it a bounce played
// is measurable from the bounce alone: three 0.4 s sections of the same tone at
// 0.6, 0.2 and 0.9 amplitude. A clip that references only part of the file must
// bounce only that part — at the level that identifies it — and a stretched one
// must bounce the same material, just slower or faster, at the expected
// timeline length. The track is hard-panned left so the left channel carries
// the material at exactly its authored amplitude (equal-power pan at hard left
// is unity), which makes the level assertions exact rather than approximate.

#include "../src/engine/Exporter.h"
#include "../src/engine/WavSource.h"
#include "../src/engine/WavWriter.h"
#include "../src/engine/Resampler.h"
#include "../src/model/ClipStretch.h"
#include "../src/model/Project.h"
#include "../src/model/Commands.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const double SR = 48000.0;
static const int64_t kFileFrames = 48000;      // 1.0 s
static const int64_t kSection     = 16000;     // 0.4 s per level section
static const float kLevels[3] = { 0.6f, 0.2f, 0.9f };

// The fixture: kLevels[i] on section i, 440 Hz, stereo, 32-bit float (exact —
// no dither, no quantisation) so a bounce's samples can be compared to the
// source's byte for byte.
static bool WriteSource(const std::string& path) {
    std::vector<float> d((size_t)kFileFrames * 2, 0.0f);
    for (int64_t i = 0; i < kFileFrames; i++) {
        const int sec = (int)(i / kSection);
        const float a = kLevels[sec > 2 ? 2 : sec];
        const float v = a * (float)std::sin(2.0 * M_PI * 440.0 * (double)i / SR);
        d[(size_t)i * 2 + 0] = v;
        d[(size_t)i * 2 + 1] = v;
    }
    WavWriter w;
    if (!w.OpenFormat(path, (int)SR, 2, 32, /*floatOut=*/true)) return false;
    if (!w.WriteFloat(d.data(), d.size(), /*dither=*/false)) return false;
    return w.Close();
}

static std::vector<float> ReadAll(const std::string& path, int64_t* frames) {
    std::vector<float> out;
    WavSource s;
    if (!s.Open(path)) { *frames = 0; return out; }
    const float* chunk = nullptr;
    size_t n = 0;
    while (s.ReadChunk(&chunk, &n))
        for (size_t i = 0; i < n * 2; i++) out.push_back(chunk[i]);
    *frames = (int64_t)(out.size() / 2);
    return out;
}

// Peak of the left channel over [from, to) output frames.
static float PeakL(const std::vector<float>& v, int64_t from, int64_t to) {
    float pk = 0.0f;
    const int64_t n = (int64_t)(v.size() / 2);
    if (from < 0) from = 0;
    if (to > n) to = n;
    for (int64_t i = from; i < to; i++)
        pk = std::max(pk, std::fabs(v[(size_t)i * 2]));
    return pk;
}

// A project with ONE audio clip of `lengthFrames` starting at source frame
// `sourceOffset`, stretched by `ratio`, hard-panned left (so L == the material).
static Project ClipProject(const std::string& src, Frame lengthFrames,
                           Frame sourceOffset, double ratio) {
    Project p;
    p.sampleRate = SR;
    p.masterGain = 1.0f;
    Track t;
    t.id   = p.NextTrackId();
    t.type = TrackType::Audio;
    t.gain = 1.0f;
    t.pan  = -1.0f;             // hard left: the equal-power law is unity there
    Clip c;
    c.startFrame   = 0;
    c.lengthFrames = lengthFrames;
    c.sourceOffset = sourceOffset;
    c.sourcePath   = src;
    c.stretchRatio = ratio;
    t.clips.push_back(c);
    p.AddTrack(t);
    return p;
}

static bool Bounce(const Project& p, const std::string& path) {
    std::remove(path.c_str());
    ExportOptions o;
    o.format.bitDepth = 32;
    o.format.dither   = false;
    return ExportWav(p, path, SR, o);
}

int main() {
    const std::string src    = "/tmp/daw_stretch_src.wav";
    const std::string out    = "/tmp/daw_stretch_out.wav";
    if (!WriteSource(src)) {
        std::printf("  FAIL could not write the fixture\n");
        return 1;
    }

    // --- 1. a stretched clip bounces to the expected timeline length and
    //        content ------------------------------------------------
    {
        // The clip's window is 1.0 s of timeline; at ratio 2.0 that is 24000
        // source frames — section 0 (0.6) and half of section 1 (0.2) — so the
        // bounce is 1.0 s long, its first two thirds play 0.6 and the rest
        // 0.2, and section 2's 0.9 never appears.
        Project p = ClipProject(src, kFileFrames, 0, 2.0);
        CHECK(Bounce(p, out));
        int64_t frames = 0;
        const std::vector<float> v = ReadAll(out, &frames);
        std::printf("    stretched bounce: %lld frames\n", (long long)frames);
        // Timeline length: the project's own end, not the source consumed.
        CHECK(frames >= kFileFrames - 2 && frames <= kFileFrames + 2);
        const float head = PeakL(v, 2000, 20000);        // source [0, 16000)
        const float tail = PeakL(v, 36000, 44000);       // source [16000, 24000)
        std::printf("    levels: %.3f %.3f\n", head, tail);
        CHECK(std::fabs(head - 0.6f) < 0.05f);
        CHECK(std::fabs(tail - 0.2f) < 0.05f);
        // Nothing that lives past the span (the 0.9 section) sounded.
        float pk = 0.0f;
        for (int64_t i = 0; i < frames; i++) pk = std::max(pk, std::fabs(v[(size_t)i * 2]));
        CHECK(pk < 0.7f);

        // The stretch is a TIME stretch, not a rate change: the tone keeps its
        // frequency (counted well inside the 0.6 section).
        int zc = 0;
        for (int64_t i = 3000; i < 17000; i++)
            if (v[(size_t)(i - 1) * 2] <= 0.0f && v[(size_t)i * 2] > 0.0f) zc++;
        const double hz = zc / (14000.0 / SR);
        std::printf("    tone: %.2f Hz\n", hz);
        CHECK(std::fabs(hz - 440.0) < 4.4);   // within ~17 cents
    }

    // --- 2. a trimmed clip: the ratio is derived from its SPAN, and the
    //        bounce consumes exactly that span ----------------------------
    {
        // The clip references source [16000, 32000) — exactly the 0.2 section —
        // and nothing else, with half the file unused on either side. Dragged
        // out to 0.5 s of timeline, the ratio must come from the trimmed span
        // (24000 / 16000 = 1.5), NOT from the file's own length (which would
        // give 48000 / 16000 = 3.0 and pull in material the user trimmed away).
        Clip c;
        c.startFrame   = 0;
        c.lengthFrames = 16000;      // 0.33 s of timeline at ratio 1.0
        c.sourceOffset = 16000;
        c.sourcePath   = src;
        const double r = StretchRatioForDrag(c, 24000, kFileFrames, SR, SR);
        std::printf("    trimmed drag ratio: %.4f\n", r);
        CHECK(std::fabs(r - 1.5) < 1e-9);

        // Apply it the way the UI does — through the command stack — and bounce.
        Project p = ClipProject(src, 16000, 16000, 1.0);
        CommandStack stack;
        CHECK(stack.Execute(std::make_unique<SetClipStretchCommand>(
                    p.Tracks().front().id, p.Tracks().front().clips.front().id,
                    24000, r), p));
        CHECK(Bounce(p, out));
        int64_t frames = 0;
        const std::vector<float> v = ReadAll(out, &frames);
        std::printf("    trimmed bounce: %lld frames\n", (long long)frames);
        CHECK(frames >= 24000 - 2 && frames <= 24000 + 2);
        // The whole bounce is the span's own material (0.2), stretched — and
        // neither the 0.6 before sourceOffset nor the 0.9 after the span
        // sounds, so the stretch consumed exactly lengthFrames / ratio source
        // frames starting at sourceOffset.
        const float a = PeakL(v, 2000, 12000);
        const float b = PeakL(v, 14000, 23000);
        std::printf("    levels: %.3f %.3f\n", a, b);
        CHECK(std::fabs(a - 0.2f) < 0.05f);
        CHECK(std::fabs(b - 0.2f) < 0.05f);
        float pk = 0.0f;
        for (int64_t i = 0; i < frames; i++) pk = std::max(pk, std::fabs(v[(size_t)i * 2]));
        CHECK(pk < 0.4f);   // 0.2 is the only material the span contains
    }

    // --- 3. the unstretched path is untouched ---------------------------
    {
        // At ratio 1.0 the bounce is the source, sample for sample: the same
        // numbers the pre-stretch exporter wrote. (This is the guard the whole
        // feature has to keep passing — see tests/exporter_tests.cpp too.)
        Project p = ClipProject(src, kFileFrames, 0, 1.0);
        CHECK(Bounce(p, out));
        int64_t frames = 0;
        const std::vector<float> v = ReadAll(out, &frames);
        CHECK(frames >= kFileFrames - 2 && frames <= kFileFrames + 2);

        // The reference: what the exporter's clip path did BEFORE the stretch
        // stage existed — decode the source at the output rate, place it at the
        // clip's timeline position with unity gain and the fade envelope. The
        // comparison is bit-for-bit, so anything the stretch work does to the
        // unstretched path (constructing a stretcher for it, dropping the
        // decode slack, shifting a frame, windowing the head) fails here.
        // (The decode goes through the same Resampler the exporter uses; at
        // equal rates it is a pass-through, and reproducing its exact output
        // keeps this a guard on the EXPORTER's path, not on the resampler's.)
        std::vector<float> want;
        {
            WavSource s;
            CHECK(s.Open(src));
            Resampler rs(s.FrameRate(), SR);
            const float* chunk = nullptr;
            size_t n = 0;
            while (s.ReadChunk(&chunk, &n)) rs.Process(chunk, n, want);
        }
        int64_t diff = 0;
        for (int64_t i = 0; i < kFileFrames && (size_t)(i * 2 + 1) < want.size(); i++)
            if (v[(size_t)i * 2] != want[(size_t)i * 2]) diff++;
        std::printf("    unstretched: %lld samples differ from the pre-stretch "
                    "renderer\n", (long long)diff);
        CHECK(diff == 0);

        // A clip stretched and then put back to 1.0 bounces identically: the
        // stretch stage leaves no trace once it is off. (Set to 1.5, then back
        // through the command, so the model really did pass through it.)
        CommandStack stack;
        Project q = ClipProject(src, kFileFrames, 0, 1.0);
        TrackId tid = q.Tracks().front().id;
        ClipId  cid = q.Tracks().front().clips.front().id;
        CHECK(stack.Execute(std::make_unique<SetClipStretchCommand>(
                    tid, cid, 16000, 1.5), q));
        CHECK(stack.Execute(std::make_unique<SetClipStretchCommand>(
                    tid, cid, kFileFrames, 1.0), q));
        CHECK(Bounce(q, out));
        int64_t frames2 = 0;
        const std::vector<float> w = ReadAll(out, &frames2);
        CHECK(frames2 == frames);
        bool same = frames2 == frames;
        for (size_t i = 0; same && i < w.size() && i < v.size(); i++)
            if (w[i] != v[i]) same = false;
        CHECK(same);
    }

    // --- 4. a hard file end: the stretch runs out of source, not off it -
    {
        // The clip's recorded span runs past the file's end. The bounce still
        // produces the clip's timeline length; what it cannot source is silence.
        Project p = ClipProject(src, 4 * kFileFrames, 0, 2.0);
        p.Tracks().front().clips.front().lengthFrames = 4 * kFileFrames;
        CHECK(Bounce(p, out));
        int64_t frames = 0;
        const std::vector<float> v = ReadAll(out, &frames);
        CHECK(frames >= 4 * kFileFrames - 2 && frames <= 4 * kFileFrames + 2);
        // The material that exists (the whole file, at half speed) sounded...
        CHECK(PeakL(v, 2000, 20000) > 0.4f);
        // ...and past the end of the material it is silence, not garbage.
        CHECK(PeakL(v, 3 * kFileFrames, 4 * kFileFrames) < 0.001f);
    }

    std::remove(src.c_str());
    std::remove(out.c_str());
    std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
