// Host-buildable tests for the Sampler.
//
// The headline property is block-size invariance: because a voice's read
// position is derived from the block's global frame, rendering a span in one
// block, in 512s, in 64s and in 37s must produce BIT-IDENTICAL output. That is
// what lets the engine rebuild its graph at a loop seam, lets a seek carry no
// voice state, and lets an offline bounce match live playback. If this test
// fails, some state leaked between blocks.

#include "../src/synth/Sampler.h"
#include "../src/synth/SfzParser.h"
#include "../src/engine/WavWriter.h"
#include "../src/model/Project.h"   // full MidiNote (Sampler.h only forward-declares it)

#include <algorithm>
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

static std::string g_dir;
static const double SR = 48000.0;

// A ramp 0..1 across `frames`, so a test can identify WHERE in the sample a
// read landed from the value it read.
// A ramp confined to [0.5, 1.0], so a zero sample can only mean a real
// dropout — the plain ramp passes through 0.0 at every loop wrap.
static bool WriteLoud(const std::string& name, int64_t frames, int rate = 48000) {
    WavWriter w;
    if (!w.OpenFormat(g_dir + "/" + name, rate, 1, 32, true))
        return false;
    std::vector<float> buf((size_t)frames);
    for (int64_t i = 0; i < frames; i++)
        buf[(size_t)i] = 0.5f + 0.5f * (float)i / (float)frames;
    const bool ok = w.WriteFloat(buf.data(), buf.size(), false);
    w.Close();
    return ok;
}

static bool WriteRamp(const std::string& name, int64_t frames, int channels = 1,
                      int rate = 48000) {
    WavWriter w;
    if (!w.OpenFormat(g_dir + "/" + name, rate, channels, 32, true))
        return false;
    std::vector<float> buf((size_t)(frames * channels));
    for (int64_t i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++)
            buf[(size_t)(i * channels + c)] =
                (float)i / (float)frames * (c == 0 ? 1.0f : -1.0f);
    const bool ok = w.WriteFloat(buf.data(), buf.size(), false);
    w.Close();
    return ok;
}

static LoadedInstrumentPtr Build(const std::string& sfz) {
    auto inst = std::make_shared<LoadedInstrument>();
    std::string err;
    if (!ParseSfzText(sfz, g_dir, inst.get(), &err))
        return nullptr;
    return inst;
}

static MidiNote Note(int pitch, int vel, Frame start, Frame len) {
    MidiNote n;
    n.pitch = pitch; n.velocity = vel; n.startFrame = start; n.lengthFrames = len;
    return n;
}

// Render `total` frames in fixed-size blocks.
static std::vector<float> RenderBlocks(const LoadedInstrumentPtr& inst,
                                       const std::vector<MidiNote>& notes,
                                       size_t total, size_t blockSize) {
    Sampler s(inst, SR);
    std::vector<float> out(total * 2, 0.0f);
    for (size_t off = 0; off < total; off += blockSize) {
        const size_t n = std::min(blockSize, total - off);
        s.Render(notes, out.data() + off * 2, n, (Frame)off, 1.0f);
    }
    return out;
}

static float Peak(const std::vector<float>& b, size_t from, size_t to) {
    float p = 0.0f;
    for (size_t i = from * 2; i < to * 2 && i < b.size(); i++)
        p = std::max(p, std::fabs(b[i]));
    return p;
}

int main() {
    char tmpl[] = "/tmp/sampler_tests_XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) { std::printf("cannot create temp dir\n"); return 1; }
    g_dir = d;

    CHECK(WriteRamp("ramp.wav", 24000));            // 0.5 s mono
    CHECK(WriteRamp("stereo.wav", 24000, 2));
    CHECK(WriteRamp("short.wav", 4800));            // 0.1 s
    CHECK(WriteLoud("loud.wav", 24000));            // never reaches 0.0

    // ---- block-size invariance (the load-bearing property) ---------------
    {
        auto inst = Build(
            "<region> sample=ramp.wav lokey=36 hikey=36 loop_mode=one_shot\n"
            "<region> sample=short.wav lokey=38 hikey=38 pitch_keycenter=38 "
            "ampeg_release=0.05\n");
        CHECK(inst && inst->regions.size() == 2);

        std::vector<MidiNote> notes = {
            Note(36, 100, 0, 4800),
            Note(38, 64, 1000, 8000),
            Note(38, 127, 9000, 2000),
        };
        const size_t total = 32000;

        const std::vector<float> whole = RenderBlocks(inst, notes, total, total);
        CHECK(Peak(whole, 0, total) > 0.0f);        // it actually made sound

        for (size_t bs : { (size_t)512, (size_t)64, (size_t)37, (size_t)1024 }) {
            const std::vector<float> split = RenderBlocks(inst, notes, total, bs);
            bool identical = (split.size() == whole.size());
            size_t firstDiff = 0;
            for (size_t i = 0; identical && i < split.size(); i++) {
                if (split[i] != whole[i]) { identical = false; firstDiff = i; }
            }
            ++g_checks;
            if (!identical) {
                ++g_fails;
                std::printf("  FAIL block size %zu diverges at sample %zu "
                            "(%g vs %g)\n", bs, firstDiff,
                            (double)split[firstDiff], (double)whole[firstDiff]);
            }
        }

        // Rendering one block in isolation must match that block's slice of the
        // whole render — this is exactly what a seek does.
        {
            Sampler s(inst, SR);
            std::vector<float> mid(512 * 2, 0.0f);
            s.Render(notes, mid.data(), 512, 5000, 1.0f);
            bool same = true;
            for (size_t i = 0; i < mid.size(); i++)
                if (mid[i] != whole[5000 * 2 + i]) { same = false; break; }
            CHECK(same);
        }
    }

    // ---- key and velocity zone selection ---------------------------------
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 lovel=0 hivel=63 loop_mode=one_shot\n"
            "<region> sample=short.wav key=36 lovel=64 hivel=127 loop_mode=one_shot\n");
        CHECK(inst && inst->regions.size() == 2);

        // A soft hit picks the long sample, a hard hit the short one; their
        // different lengths make the choice observable. one_shot so the (short)
        // note length does not cut them before the difference shows.
        const auto soft = RenderBlocks(inst, { Note(36, 32, 0, 100) }, 30000, 512);
        const auto hard = RenderBlocks(inst, { Note(36, 100, 0, 100) }, 30000, 512);
        CHECK(Peak(soft, 20000, 23000) > 0.0f);   // long sample still sounding
        CHECK(Peak(hard, 20000, 23000) == 0.0f);  // short one finished
        CHECK(Peak(hard, 1000, 2000) > 0.0f);

        // Out of range plays nothing at all.
        const auto none = RenderBlocks(inst, { Note(40, 100, 0, 100) }, 4000, 512);
        CHECK(Peak(none, 0, 4000) == 0.0f);
    }

    // ---- overlapping velocity zones LAYER, they do not pick one ---------
    // The Pettinghouse kits rely on this; picking a single best match would
    // change their sound.
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 lovel=0 hivel=93 amp_veltrack=0\n"
            "<region> sample=ramp.wav key=36 lovel=47 hivel=115 amp_veltrack=0\n");
        CHECK(inst && inst->regions.size() == 2);
        const auto one = RenderBlocks(inst, { Note(36, 30, 0, 20000) }, 20000, 512);
        const auto two = RenderBlocks(inst, { Note(36, 60, 0, 20000) }, 20000, 512);
        // Velocity 60 matches both regions, so it is twice as loud as velocity
        // 30, which matches only the first (amp_veltrack=0 removes velocity's
        // own contribution, isolating the layering).
        const float p1 = Peak(one, 10000, 11000);
        const float p2 = Peak(two, 10000, 11000);
        CHECK(p1 > 0.0f);
        CHECK(std::fabs(p2 - 2.0f * p1) < 1e-4f);
    }

    // ---- one_shot ignores note-off ---------------------------------------
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        // A 10-frame note still plays the whole 24000-frame sample.
        const auto out = RenderBlocks(inst, { Note(36, 100, 0, 10) }, 24000, 512);
        CHECK(Peak(out, 20000, 23000) > 0.0f);
    }
    {
        // Without one_shot, the same 10-frame note stops (plus a tiny release).
        auto inst = Build(
            "<region> sample=ramp.wav key=36 ampeg_release=0.001\n");
        CHECK(inst != nullptr);
        const auto out = RenderBlocks(inst, { Note(36, 100, 0, 10) }, 24000, 512);
        CHECK(Peak(out, 1000, 23000) == 0.0f);
    }

    // ---- loop wrap --------------------------------------------------------
    {
        // Loop the first 1000 frames of the ramp for a 1-second note. The ramp
        // reaches only 1000/24000 within the loop, so the peak stays low while
        // the note sounds far past the sample's own length.
        auto inst = Build(
            "<region> sample=ramp.wav key=36 loop_mode=loop_continuous "
            "loop_start=0 loop_end=999 ampeg_release=0.001\n");
        CHECK(inst != nullptr);
        // Render past note-off so the release tail is inside the buffer.
        const auto out = RenderBlocks(inst, { Note(36, 127, 0, 48000) }, 50000, 512);
        CHECK(Peak(out, 40000, 44000) > 0.0f);          // still sounding
        CHECK(Peak(out, 40000, 44000) < 0.05f);         // only the ramp's start
        CHECK(Peak(out, 48100, 50000) == 0.0f);         // released after note-off
    }

    // ---- group / off_by choke --------------------------------------------
    // An open hi-hat cut by a closed one. Without the choke the open hat rings
    // through; with it, it must be silent shortly after the closing hit.
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=46 loop_mode=one_shot group=2 off_by=1\n"
            "<region> sample=short.wav key=42 loop_mode=one_shot group=1\n");
        CHECK(inst && inst->regions.size() == 2);

        const auto open = RenderBlocks(inst, { Note(46, 100, 0, 100) }, 24000, 512);
        CHECK(Peak(open, 18000, 20000) > 0.0f);         // rings out on its own

        const std::vector<MidiNote> choked = {
            Note(46, 100, 0, 100),
            Note(42, 100, 8000, 100),                   // closed hat at 8000
        };
        const auto cut = RenderBlocks(inst, choked, 24000, 512);
        // The closed hat's own sample (4800 frames) is done by 13000, and the
        // open hat was choked at 8000, so everything past that is silent.
        CHECK(Peak(cut, 14000, 20000) == 0.0f);
        CHECK(Peak(cut, 4000, 7000) > 0.0f);            // open hat before the choke

        // The choke must not depend on block size either.
        const auto cut2 = RenderBlocks(inst, choked, 24000, 37);
        bool same = (cut.size() == cut2.size());
        for (size_t i = 0; same && i < cut.size(); i++)
            if (cut[i] != cut2[i]) same = false;
        CHECK(same);
    }

    // ---- pitch: keytrack off means drums play at their recorded pitch ----
    {
        auto inst = Build(
            "<region> sample=ramp.wav lokey=36 hikey=48 pitch_keycenter=36 "
            "pitch_keytrack=0 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        const auto low  = RenderBlocks(inst, { Note(36, 100, 0, 100) }, 24000, 512);
        const auto high = RenderBlocks(inst, { Note(48, 100, 0, 100) }, 24000, 512);
        bool same = true;
        for (size_t i = 0; i < low.size(); i++)
            if (low[i] != high[i]) { same = false; break; }
        CHECK(same);
    }
    {
        // With keytrack on, an octave up consumes the sample twice as fast.
        auto inst = Build(
            "<region> sample=ramp.wav lokey=36 hikey=48 pitch_keycenter=36 "
            "loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        const auto high = RenderBlocks(inst, { Note(48, 100, 0, 100) }, 24000, 512);
        CHECK(Peak(high, 0, 12000) > 0.0f);
        CHECK(Peak(high, 13000, 24000) == 0.0f);   // finished at half the length
    }

    // ---- stereo samples keep their channels ------------------------------
    {
        auto inst = Build(
            "<region> sample=stereo.wav key=36 pan=0 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        CHECK(inst->samples.size() == 1 && inst->samples[0].channels == 2);
        const auto out = RenderBlocks(inst, { Note(36, 127, 0, 100) }, 24000, 512);
        // The fixture's right channel is the negated left, so the two sides
        // must have opposite sign at the same frame.
        const size_t probe = 12000 * 2;
        CHECK(out[probe] > 0.0f);
        CHECK(out[probe + 1] < 0.0f);
    }

    // ---- pan ---------------------------------------------------------------
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 pan=-100 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        const auto out = RenderBlocks(inst, { Note(36, 127, 0, 100) }, 24000, 512);
        const size_t probe = 12000 * 2;
        CHECK(std::fabs(out[probe]) > 0.0f);
        CHECK(std::fabs(out[probe + 1]) < 1e-6f);   // nothing on the right
    }

    // ---- gain ramp continuity (matches Synth's de-zipper contract) -------
    {
        auto inst = Build("<region> sample=ramp.wav key=36 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        Sampler s(inst, SR);
        std::vector<float> out(1024 * 2, 0.0f);
        s.Render({ Note(36, 127, 0, 24000) }, out.data(), 1024, 0,
                 StereoGain{0.0f, 0.0f}, StereoGain{1.0f, 1.0f});
        // Ramped from silence, so late samples must exceed early ones.
        CHECK(std::fabs(out[0]) < std::fabs(out[1023 * 2]));
    }

    // ---- an empty / null instrument renders silence, it does not crash ---
    {
        Sampler none(nullptr, SR);
        std::vector<float> out(512 * 2, 0.0f);
        none.Render({ Note(36, 100, 0, 1000) }, out.data(), 512, 0, 1.0f);
        CHECK(Peak(out, 0, 512) == 0.0f);

        auto empty = std::make_shared<LoadedInstrument>();
        Sampler es(empty, SR);
        es.Render({ Note(36, 100, 0, 1000) }, out.data(), 512, 0, 1.0f);
        CHECK(Peak(out, 0, 512) == 0.0f);
    }

    // ---- additive: the sampler never clears the caller's buffer ----------
    {
        auto inst = Build("<region> sample=ramp.wav key=36 loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        Sampler s(inst, SR);
        std::vector<float> out(512 * 2, 0.25f);
        s.Render({ Note(36, 100, 0, 1000) }, out.data(), 512, 0, 1.0f);
        bool allChanged = true;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i] == 0.0f) { allChanged = false; break; }
        CHECK(allChanged);   // the pre-existing 0.25 was added to, not wiped
    }

    // ---- REGRESSION: the voice cap must not allocate, bias, or reorder ---
    // fVoices used to reserve kMaxVoices but push up to 2*kMaxVoices before
    // breaking, so the RT thread reallocated mid-block; and the break truncated
    // by scan order, so the "most recent wins" rule kept the wrong voices.
    {
        auto inst = Build("<region> sample=ramp.wav lokey=0 hikey=127 "
                          "pitch_keytrack=0 loop_mode=one_shot\n");
        CHECK(inst != nullptr);

        std::vector<MidiNote> many;
        for (int i = 0; i < Sampler::kMaxVoices * 3; i++)
            many.push_back(Note(36 + (i % 40), 100, i * 2, 20000));

        Sampler s(inst, SR);
        std::vector<float> out(4096 * 2, 0.0f);
        s.Render(many, out.data(), 4096, 0, 1.0f);
        CHECK(Peak(out, 0, 4096) > 0.0f);

        // Deterministic: the same candidate set must render identically.
        const auto a1 = RenderBlocks(inst, many, 8192, 8192);
        const auto a2 = RenderBlocks(inst, many, 8192, 8192);
        bool same = (a1.size() == a2.size());
        for (size_t i = 0; same && i < a1.size(); i++)
            if (a1[i] != a2[i]) same = false;
        CHECK(same);
    }

    // ---- REGRESSION: a looping region must not click once per loop --------
    // When loop_end defaults to the sample end, the fmod wrap could land in
    // (loopEnd, loopEnd+1), which the non-looping "played out" test then
    // rejected — dropping one frame every loop period.
    {
        auto inst = Build(
            "<region> sample=loud.wav key=36 loop_mode=loop_continuous "
            "ampeg_release=0.001\n");
        CHECK(inst != nullptr);
        const auto out = RenderBlocks(inst, { Note(36, 127, 0, 96000) }, 96000, 512);
        int dropouts = 0;
        for (size_t i = 100; i < 90000; i++)
            if (out[i * 2] == 0.0f && out[i * 2 + 1] == 0.0f) dropouts++;
        ++g_checks;
        if (dropouts != 0) {
            ++g_fails;
            std::printf("  FAIL loop wrap dropped %d frames\n", dropouts);
        }
    }

    // ---- REGRESSION: an absurd volume must not reach the mix as Inf ------
    {
        auto inst = Build("<region> sample=ramp.wav key=36 volume=10000 "
                          "loop_mode=one_shot\n");
        CHECK(inst != nullptr);
        const auto out = RenderBlocks(inst, { Note(36, 127, 0, 4800) }, 8192, 512);
        bool finite = true;
        for (float v : out) if (!std::isfinite(v)) { finite = false; break; }
        CHECK(finite);
    }

    // ---- REGRESSION: absurd tune/delay must not be UB or hang ------------
    {
        for (const char* src : {
                "<region> sample=ramp.wav key=36 tune=-1000000\n",
                "<region> sample=ramp.wav key=36 delay=1e30\n",
                "<region> sample=ramp.wav key=36 ampeg_release=1e30\n",
                "<region> sample=ramp.wav key=36 pitch_keytrack=-100000\n" }) {
            auto inst = Build(src);
            CHECK(inst != nullptr);
            std::vector<float> out(1024 * 2, 0.0f);
            Sampler s(inst, SR);
            s.Render({ Note(36, 100, 0, 4800) }, out.data(), 1024, 0, 1.0f);
            bool finite = true;
            for (float v : out) if (!std::isfinite(v)) { finite = false; break; }
            CHECK(finite);
        }
    }

    // ---- random round-robin (lorand/hirand) ------------------------------
    // Without this, every take of a hit layers at once. Swirly Drums uses it
    // on 2205 regions.
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 lorand=0.0 hirand=0.5 "
            "loop_mode=one_shot amp_veltrack=0\n"
            "<region> sample=ramp.wav key=36 lorand=0.5 hirand=1.0 "
            "loop_mode=one_shot amp_veltrack=0\n");
        CHECK(inst && inst->regions.size() == 2);
        CHECK(inst->hasRandom);

        auto single = Build("<region> sample=ramp.wav key=36 "
                            "loop_mode=one_shot amp_veltrack=0\n");
        const auto ref = RenderBlocks(single, { Note(36, 100, 0, 4800) }, 24000, 512);
        const float refPk = Peak(ref, 0, 24000);
        CHECK(refPk > 0.0f);

        int sounded = 0;
        for (int k = 0; k < 24; k++) {
            const auto out = RenderBlocks(inst, { Note(36, 100, k * 1000, 4800) },
                                          40000, 512);
            const float pk = Peak(out, 0, 40000);
            if (pk > 0.0f) sounded++;
            CHECK(pk < refPk * 1.5f);   // one take, not both layered
        }
        CHECK(sounded == 24);

        // Same note -> same take, at any block size (stateless determinism).
        const auto r1 = RenderBlocks(inst, { Note(36, 100, 5000, 4800) }, 40000, 512);
        const auto r2 = RenderBlocks(inst, { Note(36, 100, 5000, 4800) }, 40000, 64);
        bool same = (r1.size() == r2.size());
        for (size_t i = 0; same && i < r1.size(); i++)
            if (r1[i] != r2[i]) same = false;
        CHECK(same);
    }

    // ---- sequential round-robin (seq_length / seq_position) --------------
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 seq_length=2 seq_position=1 "
            "loop_mode=one_shot\n"
            "<region> sample=short.wav key=36 seq_length=2 seq_position=2 "
            "loop_mode=one_shot\n");
        CHECK(inst && inst->regions.size() == 2);
        CHECK(inst->hasSeq);
        const std::vector<MidiNote> two = {
            Note(36, 100, 0, 1000), Note(36, 100, 30000, 1000) };
        const auto out = RenderBlocks(inst, two, 60000, 512);
        CHECK(Peak(out, 20000, 23000) > 0.0f);   // long sample still ringing
        CHECK(Peak(out, 30500, 34000) > 0.0f);   // short sample fired on hit 2
        CHECK(Peak(out, 36000, 60000) == 0.0f);  // and finished early
    }

    // ---- loop_sustain leaves the loop at note-off ------------------------
    // It used to be treated as loop_continuous. The audible difference is not
    // whether the voice sounds during the release — the amp envelope governs
    // that — but WHAT it plays: a continuous loop keeps repeating the loop
    // region, while a sustain loop runs forward through the rest of the sample.
    //
    // loud.wav ramps 0.5 -> 1.0 across 24000 frames and the loop covers only
    // its first 1000, so the loop content sits at ~0.5 while the material past
    // it climbs toward 1.0. A long release keeps both audible so the contents
    // can be compared.
    {
        auto sus = Build(
            "<region> sample=loud.wav key=36 loop_mode=loop_sustain "
            "loop_start=0 loop_end=999 ampeg_release=1.0 amp_veltrack=0\n");
        auto cont = Build(
            "<region> sample=loud.wav key=36 loop_mode=loop_continuous "
            "loop_start=0 loop_end=999 ampeg_release=1.0 amp_veltrack=0\n");
        CHECK(sus != nullptr && cont != nullptr);
        CHECK(sus->regions.size() == 1 && sus->regions[0].loopMode == LoopMode::LoopSustain);

        const std::vector<MidiNote> one = { Note(36, 127, 0, 24000) };
        const auto a2 = RenderBlocks(sus,  one, 90000, 512);
        const auto b2 = RenderBlocks(cont, one, 90000, 512);

        // While held, both are inside the loop: same low-amplitude content.
        CHECK(Peak(a2, 10000, 20000) > 0.0f);
        CHECK(Peak(b2, 10000, 20000) > 0.0f);
        CHECK(std::fabs(Peak(a2, 10000, 20000) - Peak(b2, 10000, 20000)) < 1e-4f);

        // Just after note-off both still sound (1 s release)...
        CHECK(Peak(a2, 26000, 30000) > 0.0f);
        CHECK(Peak(b2, 26000, 30000) > 0.0f);
        // ...but the sustain voice has left the loop and is playing later,
        // louder material, while the continuous one is still in the loop.
        CHECK(Peak(a2, 40000, 46000) > Peak(b2, 40000, 46000) * 1.2f);

        // Still block-size invariant with the new release path.
        const auto a3 = RenderBlocks(sus, one, 90000, 37);
        bool same = (a2.size() == a3.size());
        for (size_t i = 0; same && i < a2.size(); i++)
            if (a2[i] != a3[i]) same = false;
        CHECK(same);
    }

    // ---- the per-note window early-out must not truncate anything -------
    // maxTailSeconds now gates whether a note is considered for a block; if it
    // under-estimates, long tails get cut.
    {
        auto inst = Build(
            "<region> sample=ramp.wav key=36 loop_mode=one_shot\n"
            "<region> sample=loud.wav key=38 loop_mode=loop_sustain "
            "loop_start=0 loop_end=999 ampeg_release=0.5\n");
        CHECK(inst != nullptr);
        const std::vector<MidiNote> notes = {
            Note(36, 100, 0, 10), Note(38, 100, 5000, 8000) };
        const auto whole = RenderBlocks(inst, notes, 80000, 80000);
        const auto split = RenderBlocks(inst, notes, 80000, 512);
        bool same = (whole.size() == split.size());
        for (size_t i = 0; same && i < whole.size(); i++)
            if (whole[i] != split[i]) same = false;
        CHECK(same);
        CHECK(Peak(split, 20000, 24000) > 0.0f);   // one-shot tail survives
    }

    // ---- REGRESSION: ampeg_delay pushes the voice later, so it must count
    // toward maxTailSeconds too. It was omitted (only `delay` was folded in),
    // so the per-note window early-out rejected the note long before the
    // delayed voice was due and the region never sounded at all.
    {
        auto inst = Build(
            "<region> sample=short.wav key=36 loop_mode=one_shot "
            "ampeg_delay=1.0\n");
        CHECK(inst != nullptr);
        // 1 s delay at 48 kHz: the 4800-frame sample sounds in [48000, 52800).
        const std::vector<MidiNote> notes = { Note(36, 100, 0, 100) };
        const auto out = RenderBlocks(inst, notes, 80000, 512);
        CHECK(Peak(out, 0, 47000) == 0.0f);         // silent before the delay
        CHECK(Peak(out, 48100, 52000) > 0.0f);      // and sounds after it
        // The whole-buffer render takes the same path, so they must agree.
        const auto whole = RenderBlocks(inst, notes, 80000, 80000);
        bool same = (whole.size() == out.size());
        for (size_t i = 0; same && i < whole.size(); i++)
            if (whole[i] != out[i]) same = false;
        CHECK(same);
    }
    {
        // The same for the plain `delay` opcode, which WAS counted — a guard so
        // the fix above cannot regress it.
        auto inst = Build(
            "<region> sample=short.wav key=36 loop_mode=one_shot delay=1.0\n");
        CHECK(inst != nullptr);
        const auto out = RenderBlocks(inst, { Note(36, 100, 0, 100) }, 80000, 512);
        CHECK(Peak(out, 48100, 52000) > 0.0f);
    }

    std::system(("rm -rf '" + g_dir + "'").c_str());

    std::printf("sampler_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
