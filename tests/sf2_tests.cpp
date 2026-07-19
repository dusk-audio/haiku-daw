// Host-buildable tests for the SF2 reader and the SF2 -> region converter.
//
// The fixture is a minimal .sf2 synthesised byte-by-byte here rather than a
// checked-in binary, so the tests are self-contained and can be MUTATED to
// exercise the hardening paths: a crafted shdr end value must be refused
// before it sizes an allocation (a declared end of 0x7FFFFFFF would otherwise
// ask for ~4 GB), and a truncated file must fail cleanly.

#include "../src/synth/Sf2Reader.h"
#include "../src/synth/Sf2ToRegions.h"
#include "../src/synth/Sampler.h"
#include "../src/model/Project.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// ------------------------------------------------------------- SF2 builder --

namespace {

struct Buf {
    std::vector<uint8_t> b;
    void u8(int v)  { b.push_back((uint8_t)v); }
    void u16(int v) { u8(v & 0xff); u8((v >> 8) & 0xff); }
    void u32(uint32_t v) { u8(v & 0xff); u8((v >> 8) & 0xff);
                           u8((v >> 16) & 0xff); u8((v >> 24) & 0xff); }
    void id(const char* s) { for (int i = 0; i < 4; i++) u8(s[i]); }
    void name20(const char* s) {
        // Stop at the string's terminator, then NUL-pad the rest of the field.
        // Indexing s[i] all the way to 19 read past the end of a short literal
        // like "EOP" — out of bounds, and ASan flags it.
        int i = 0;
        for (; i < 20 && s[i]; i++) u8(s[i]);
        for (; i < 20; i++) u8(0);
    }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
    size_t size() const { return b.size(); }
};

// Fix up a chunk's u32 size field, written as a placeholder at `sizePos`.
void patchSize(Buf& f, size_t sizePos, size_t bodyStart) {
    const uint32_t n = (uint32_t)(f.b.size() - bodyStart);
    f.b[sizePos + 0] = (uint8_t)(n & 0xff);
    f.b[sizePos + 1] = (uint8_t)((n >> 8) & 0xff);
    f.b[sizePos + 2] = (uint8_t)((n >> 16) & 0xff);
    f.b[sizePos + 3] = (uint8_t)((n >> 24) & 0xff);
}

struct Sf2Spec {
    int      sampleFrames  = 200;
    uint32_t shdrStart     = 0;
    uint32_t shdrEnd       = 200;    // set absurd to test the bounds check
    uint32_t startLoop     = 20;
    uint32_t endLoop       = 180;
    int      loKey         = 60;
    int      hiKey         = 72;
    int      rootKey       = 60;
    int      sampleModes   = 1;      // 1 = loop continuously
    int      exclusiveClass = 0;
    int      attenuationCb = 0;
    int      panRaw        = 0;      // -500..500; 0 = centre (no gen emitted)
    bool     emitPan       = false;
    int      scaleTuning   = 100;
};

// Build a minimal but spec-shaped SF2: one preset -> one instrument -> one
// sample, plus the terminal sentinel records the format requires.
std::vector<uint8_t> BuildSf2(const Sf2Spec& sp) {
    Buf f;
    f.id("RIFF");
    const size_t riffSizePos = f.size();
    f.u32(0);
    const size_t riffBody = f.size();
    f.id("sfbk");

    // ---- LIST sdta { smpl } ----
    f.id("LIST");
    const size_t sdtaSizePos = f.size();
    f.u32(0);
    const size_t sdtaBody = f.size();
    f.id("sdta");
    f.id("smpl");
    f.u32((uint32_t)(sp.sampleFrames * 2));
    for (int i = 0; i < sp.sampleFrames; i++) {
        // A full-scale sine so a rendered note is unmistakably non-silent.
        const double t = (double)i / sp.sampleFrames;
        const int16_t v = (int16_t)(std::sin(t * 2.0 * M_PI * 4.0) * 30000.0);
        f.u16((uint16_t)v);
    }
    patchSize(f, sdtaSizePos, sdtaBody);

    // ---- LIST pdta { phdr pbag pgen inst ibag igen shdr } ----
    f.id("LIST");
    const size_t pdtaSizePos = f.size();
    f.u32(0);
    const size_t pdtaBody = f.size();
    f.id("pdta");

    // phdr: one preset + terminal EOP. 38 bytes each.
    f.id("phdr"); f.u32(38 * 2);
    f.name20("Test Preset"); f.u16(0); f.u16(0); f.u16(0);
    f.u32(0); f.u32(0); f.u32(0);
    f.name20("EOP");         f.u16(0); f.u16(0); f.u16(1);
    f.u32(0); f.u32(0); f.u32(0);

    // pbag: zone 0 starts at pgen 0; terminal bag bounds it at pgen 1.
    f.id("pbag"); f.u32(4 * 2);
    f.u16(0); f.u16(0);
    f.u16(1); f.u16(0);

    // pgen: the preset zone's single generator -> instrument 0.
    f.id("pgen"); f.u32(4 * 1);
    f.u16(kGenInstrument); f.u16(0);

    // inst: one instrument + terminal EOI. 22 bytes each.
    f.id("inst"); f.u32(22 * 2);
    f.name20("Test Inst"); f.u16(0);
    f.name20("EOI");       f.u16(0);   // patched below to bound the zone

    // The EOI record's bagNdx must point past the real instrument's zones.
    // Its u16 sits at the last two bytes written above.
    f.b[f.b.size() - 2] = 1;
    f.b[f.b.size() - 1] = 0;

    // ibag: instrument zone 0 spans igen [0, N).
    int nIgen = 3;   // keyRange, sampleModes, sampleID
    if (sp.exclusiveClass) nIgen++;
    if (sp.attenuationCb)  nIgen++;
    if (sp.emitPan)        nIgen++;
    if (sp.scaleTuning != 100) nIgen++;
    f.id("ibag"); f.u32(4 * 2);
    f.u16(0);            f.u16(0);
    f.u16((uint16_t)nIgen); f.u16(0);

    // igen: sampleID MUST come last — the spec makes it the zone's terminal
    // generator, and the reader uses its presence to tell a real zone from a
    // global one.
    f.id("igen"); f.u32((uint32_t)(4 * nIgen));
    f.u16(kGenKeyRange); f.u16((uint16_t)((sp.hiKey << 8) | sp.loKey));
    if (sp.attenuationCb)  { f.u16(kGenInitialAttenuation); f.u16((uint16_t)sp.attenuationCb); }
    if (sp.emitPan)        { f.u16(kGenPan); f.u16((uint16_t)(int16_t)sp.panRaw); }
    if (sp.exclusiveClass) { f.u16(kGenExclusiveClass); f.u16((uint16_t)sp.exclusiveClass); }
    if (sp.scaleTuning != 100) { f.u16(kGenScaleTuning); f.u16((uint16_t)sp.scaleTuning); }
    f.u16(kGenSampleModes); f.u16((uint16_t)sp.sampleModes);
    f.u16(kGenSampleID);    f.u16(0);

    // shdr: one sample + terminal EOS. 46 bytes each.
    f.id("shdr"); f.u32(46 * 2);
    f.name20("TestSample");
    f.u32(sp.shdrStart); f.u32(sp.shdrEnd);
    f.u32(sp.startLoop); f.u32(sp.endLoop);
    f.u32(44100); f.u8(sp.rootKey); f.u8(0); f.u16(0); f.u16(1);
    f.name20("EOS");
    f.u32(0); f.u32(0); f.u32(0); f.u32(0);
    f.u32(0); f.u8(0); f.u8(0); f.u16(0); f.u16(0);

    patchSize(f, pdtaSizePos, pdtaBody);
    patchSize(f, riffSizePos, riffBody);
    return f.b;
}

std::string g_dir;

std::string WriteSf2(const std::string& name, const std::vector<uint8_t>& data,
                     size_t truncateTo = 0) {
    const std::string p = g_dir + "/" + name;
    std::ofstream o(p, std::ios::binary);
    const size_t n = (truncateTo && truncateTo < data.size()) ? truncateTo : data.size();
    o.write((const char*)data.data(), (std::streamsize)n);
    return p;
}

float RenderPeak(const LoadedInstrumentPtr& inst, int pitch, int vel) {
    Sampler s(inst, 48000.0);
    std::vector<float> out(24000 * 2, 0.0f);
    MidiNote n; n.pitch = pitch; n.velocity = vel;
    n.startFrame = 0; n.lengthFrames = 12000;
    const std::vector<MidiNote> notes = { n };
    for (size_t off = 0; off < 24000; off += 512) {
        const size_t k = std::min<size_t>(512, 24000 - off);
        s.Render(notes, out.data() + off * 2, k, (Frame)off, 1.0f);
    }
    float pk = 0.0f;
    for (float v : out) pk = std::max(pk, std::fabs(v));
    return pk;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/sf2_tests_XXXXXX";
    const char* d = mkdtemp(tmpl);
    if (!d) { std::printf("cannot create temp dir\n"); return 1; }
    g_dir = d;

    // ---- a well-formed minimal bank parses ------------------------------
    {
        const std::string p = WriteSf2("ok.sf2", BuildSf2(Sf2Spec{}));
        const Sf2File f = ReadSf2(p);
        CHECK(f.ok);
        CHECK(f.error.empty());
        CHECK(f.presets.size() == 1);          // the EOP sentinel is dropped
        CHECK(f.instruments.size() == 1);      // ...and EOI
        CHECK(f.samples.size() == 1);          // ...and EOS
        if (f.presets.size() == 1)
            CHECK(f.presets[0].name == "Test Preset");
        if (f.samples.size() == 1) {
            CHECK(f.samples[0].name == "TestSample");
            CHECK(f.samples[0].sampleRate == 44100);
            CHECK(f.samples[0].end == 200);
        }
        CHECK(f.smplSize == 400);              // 200 frames of 16-bit PCM
    }

    // ---- conversion produces a playable region --------------------------
    {
        const std::string p = WriteSf2("conv.sf2", BuildSf2(Sf2Spec{}));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        CHECK(err.empty());
        CHECK(inst.regions.size() == 1);
        CHECK(inst.samples.size() == 1);
        if (inst.regions.size() == 1) {
            const Region& r = inst.regions[0];
            CHECK(r.loKey == 60 && r.hiKey == 72);
            CHECK(r.pitchKeycenter == 60);
            CHECK(r.loopMode == LoopMode::LoopContinuous);
            // Loop points are absolute into the smpl chunk and must be re-based
            // onto the extracted sample.
            CHECK(r.loopStart == 20);
            CHECK(r.loopEnd == 179);
            CHECK(r.pitchKeytrack == 100.0f);
        }
        if (inst.samples.size() == 1) {
            CHECK(inst.samples[0].channels == 1);
            CHECK(inst.samples[0].frames == 200);
            CHECK(inst.samples[0].sampleRate == 44100.0);
        }
        auto ptr = std::make_shared<LoadedInstrument>(inst);
        CHECK(RenderPeak(ptr, 60, 100) > 0.0f);
        CHECK(RenderPeak(ptr, 90, 100) == 0.0f);   // outside the key range
    }

    // ---- hardening: a crafted shdr end must be REFUSED ------------------
    // The bounds check exists because `end` is file-declared: 0x7FFFFFFF would
    // request a ~4 GB buffer, and frames*2 in int is signed-overflow UB.
    {
        Sf2Spec sp;
        sp.shdrEnd = 0x7FFFFFFF;
        const std::string p = WriteSf2("evil.sf2", BuildSf2(sp));
        const Sf2File f = ReadSf2(p);
        CHECK(f.ok);                            // metadata still parses...
        LoadedInstrument inst;
        std::string err;
        CHECK(!LoadSf2Preset(p, 0, &inst, &err));   // ...but nothing is extracted
        CHECK(inst.regions.empty());
        CHECK(inst.samples.empty());
    }
    {
        // end beyond the smpl chunk but small enough to look plausible.
        Sf2Spec sp;
        sp.shdrEnd = 100000;
        const std::string p = WriteSf2("over.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(!LoadSf2Preset(p, 0, &inst, &err));
        CHECK(inst.regions.empty());
    }
    {
        // end <= start is degenerate and must not produce a zero/negative span.
        Sf2Spec sp;
        sp.shdrStart = 100; sp.shdrEnd = 100;
        const std::string p = WriteSf2("empty.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(!LoadSf2Preset(p, 0, &inst, &err));
    }

    // ---- truncated files fail cleanly, at several cut points ------------
    {
        const std::vector<uint8_t> full = BuildSf2(Sf2Spec{});
        for (size_t cut : { (size_t)4, (size_t)12, (size_t)40, full.size() / 2,
                            full.size() - 20 }) {
            const std::string p = WriteSf2("trunc.sf2", full, cut);
            const Sf2File f = ReadSf2(p);
            // Either it refuses, or it parses whatever survived — but it must
            // not claim success with a sample it cannot actually read.
            LoadedInstrument inst;
            std::string err;
            const bool loaded = LoadSf2Preset(p, 0, &inst, &err);
            ++g_checks;
            // Success must mean COMPLETE, usable output: regions present, every
            // one indexing a real sample that actually holds decoded audio.
            // Failure must leave nothing behind for a caller to misread.
            std::string why;
            if (loaded) {
                if (inst.regions.empty()) why = "no regions";
                else if (inst.samples.empty()) why = "no samples";
                for (const Region& r : inst.regions) {
                    if (r.sampleIndex < 0
                        || (size_t)r.sampleIndex >= inst.samples.size()) {
                        why = "region indexes a missing sample";
                        break;
                    }
                    const SampleData& sd = inst.samples[(size_t)r.sampleIndex];
                    if (sd.frames <= 0 || sd.data.empty()) {
                        why = "region's sample has no audio";
                        break;
                    }
                    if (r.end >= sd.frames || r.offset >= sd.frames) {
                        why = "region span outside its sample";
                        break;
                    }
                }
            } else if (!inst.regions.empty() || !inst.samples.empty()) {
                why = "failed load left output behind";
            }
            if (!why.empty()) {
                ++g_fails;
                std::printf("  FAIL truncation at %zu: %s\n", cut, why.c_str());
            }
            (void)f;
        }
    }

    // ---- not an SF2 at all ----------------------------------------------
    {
        const std::string p = g_dir + "/notsf2.sf2";
        { std::ofstream o(p, std::ios::binary); o << "hello, this is not a soundfont"; }
        const Sf2File f = ReadSf2(p);
        CHECK(!f.ok);
        CHECK(!f.error.empty());
        LoadedInstrument inst;
        std::string err;
        CHECK(!LoadSf2Preset(p, 0, &inst, &err));
    }
    {
        const Sf2File f = ReadSf2(g_dir + "/does_not_exist.sf2");
        CHECK(!f.ok);
    }

    // ---- generator semantics --------------------------------------------
    {
        // exclusiveClass becomes a self-choking group (drum hi-hats).
        Sf2Spec sp; sp.exclusiveClass = 5;
        const std::string p = WriteSf2("excl.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        if (inst.regions.size() == 1) {
            CHECK(inst.regions[0].group == 5);
            CHECK(inst.regions[0].offBy == 5);   // choked BY its own class
        }
    }
    {
        // initialAttenuation is centibels of cut: 100 cB -> -10 dB.
        Sf2Spec sp; sp.attenuationCb = 100;
        const std::string p = WriteSf2("atten.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        if (inst.regions.size() == 1)
            CHECK(std::fabs(inst.regions[0].volumeDb - (-10.0f)) < 1e-4f);
    }
    {
        // pan generator is -500..500 mapping to -100..100.
        Sf2Spec sp; sp.emitPan = true; sp.panRaw = -500;
        const std::string p = WriteSf2("pan.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        if (inst.regions.size() == 1)
            CHECK(std::fabs(inst.regions[0].pan - (-100.0f)) < 1e-4f);
    }
    {
        // scaleTuning 0 = no key tracking, which is how drum banks map.
        Sf2Spec sp; sp.scaleTuning = 0;
        const std::string p = WriteSf2("drum.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        if (inst.regions.size() == 1)
            CHECK(inst.regions[0].pitchKeytrack == 0.0f);
    }
    {
        // sampleModes 0 = no loop.
        Sf2Spec sp; sp.sampleModes = 0;
        const std::string p = WriteSf2("noloop.sf2", BuildSf2(sp));
        LoadedInstrument inst;
        std::string err;
        CHECK(LoadSf2Preset(p, 0, &inst, &err));
        if (inst.regions.size() == 1)
            CHECK(inst.regions[0].loopMode == LoopMode::NoLoop);
    }

    // ---- preset listing --------------------------------------------------
    {
        const std::string p = WriteSf2("list.sf2", BuildSf2(Sf2Spec{}));
        std::string err;
        const auto presets = ListSf2Presets(p, &err);
        CHECK(presets.size() == 1);
        if (presets.size() == 1) {
            CHECK(presets[0].name == "Test Preset");
            CHECK(presets[0].index == 0);
        }
        // An out-of-range preset index is clamped, not rejected.
        LoadedInstrument inst;
        std::string e2;
        CHECK(LoadSf2Preset(p, 999, &inst, &e2));
        CHECK(inst.regions.size() == 1);
    }

    // ---- HARDENING: zone x generator explosion ---------------------------
    // bagNdx and genNdx are each bounds-checked, but nothing bounded their
    // PRODUCT: alternating 0/65535 indices made every instrument build ~32k
    // zones of ~65k generators. Measured at 6 GB before dying. Must now
    // complete quickly and bounded, from metadata parsing alone.
    {
        Buf f;
        f.id("RIFF");
        const size_t riffSizePos = f.size();
        f.u32(0);
        const size_t riffBody = f.size();
        f.id("sfbk");

        f.id("LIST");
        const size_t sdtaSizePos = f.size();
        f.u32(0);
        const size_t sdtaBody = f.size();
        f.id("sdta");
        f.id("smpl"); f.u32(400);
        for (int i = 0; i < 200; i++) f.u16(0);
        patchSize(f, sdtaSizePos, sdtaBody);

        f.id("LIST");
        const size_t pdtaSizePos = f.size();
        f.u32(0);
        const size_t pdtaBody = f.size();
        f.id("pdta");

        // 40 presets, each pointing at alternating bag indices.
        const int nPre = 40;
        f.id("phdr"); f.u32((uint32_t)(38 * (nPre + 1)));
        for (int i = 0; i <= nPre; i++) {
            f.name20(i == nPre ? "EOP" : "P");
            f.u16(i); f.u16(0);
            f.u16((uint16_t)((i % 2) ? 0 : 65535));   // adversarial bagNdx
            f.u32(0); f.u32(0); f.u32(0);
        }
        // 65536 bags with alternating genNdx.
        f.id("pbag"); f.u32((uint32_t)(4 * 65536));
        for (int i = 0; i < 65536; i++) {
            f.u16((uint16_t)((i % 2) ? 0 : 65535));
            f.u16(0);
        }
        f.id("pgen"); f.u32((uint32_t)(4 * 65535));
        for (int i = 0; i < 65535; i++) { f.u16(kGenInstrument); f.u16(0); }

        const int nInst = 40;
        f.id("inst"); f.u32((uint32_t)(22 * (nInst + 1)));
        for (int i = 0; i <= nInst; i++) {
            f.name20(i == nInst ? "EOI" : "I");
            f.u16((uint16_t)((i % 2) ? 0 : 65535));
        }
        f.id("ibag"); f.u32((uint32_t)(4 * 65536));
        for (int i = 0; i < 65536; i++) {
            f.u16((uint16_t)((i % 2) ? 0 : 65535));
            f.u16(0);
        }
        f.id("igen"); f.u32((uint32_t)(4 * 65535));
        for (int i = 0; i < 65535; i++) { f.u16(kGenSampleID); f.u16(0); }

        f.id("shdr"); f.u32(46 * 2);
        f.name20("S");
        f.u32(0); f.u32(200); f.u32(0); f.u32(0);
        f.u32(44100); f.u8(60); f.u8(0); f.u16(0); f.u16(1);
        f.name20("EOS");
        f.u32(0); f.u32(0); f.u32(0); f.u32(0);
        f.u32(0); f.u8(0); f.u8(0); f.u16(0); f.u16(0);

        patchSize(f, pdtaSizePos, pdtaBody);
        patchSize(f, riffSizePos, riffBody);

        const std::string p = WriteSf2("bomb.sf2", f.b);
        const auto t0 = std::chrono::steady_clock::now();
        const Sf2File parsed = ReadSf2(p);
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        ++g_checks;
        if (ms > 5000.0) {
            ++g_fails;
            std::printf("  FAIL zone explosion took %.0f ms\n", ms);
        }
        // Listing presets is what the instrument window does on file pick —
        // it must survive this too.
        std::string err;
        const auto t1 = std::chrono::steady_clock::now();
        const auto presets = ListSf2Presets(p, &err);
        const double ms2 = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t1).count();
        ++g_checks;
        if (ms2 > 5000.0) {
            ++g_fails;
            std::printf("  FAIL preset listing took %.0f ms\n", ms2);
        }
        (void)parsed; (void)presets;
    }

    std::system(("rm -rf '" + g_dir + "'").c_str());

    std::printf("sf2_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
