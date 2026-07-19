// Host-buildable tests for InstrumentDesc persistence.
//
// The format rule under test: the `instrument` line keeps its original five
// fields and is ALWAYS written, and a soundfont adds a separate optional
// `soundfont` line. That way a project saved by this build still opens in an
// older one (playing the synth voice), and a project saved by an older build
// still opens here.

#include "../src/model/ProjectIO.h"
#include "../src/model/Commands.h"
#include "../src/synth/InstrumentFactory.h"
#include "../src/synth/SampleBank.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

static const char* kPath = "instrument_io_test_tmp.dawproj";

static std::string ReadFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Save a project holding one MIDI track with `desc`, rewrite the saved TEXT
// (to stand in for a file written by an older or newer build), then load it.
// Patching a real save beats hand-writing the format, which has a dozen
// fields on the track line alone.
static InstrumentDesc LoadPatched(const InstrumentDesc& desc,
                                  const std::string& from,
                                  const std::string& to) {
    Project a;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Keys"), a);
    a.Tracks().front().instrument = desc;
    ProjectIO::Save(a, kPath);

    std::string text = ReadFile(kPath);
    const size_t at = text.find(from);
    if (at != std::string::npos)
        text.replace(at, from.size(), to);
    { std::ofstream f(kPath); f << text; }

    Project b;
    if (!ProjectIO::Load(b, kPath) || b.Tracks().empty())
        return InstrumentDesc{};
    return b.Tracks().front().instrument;
}

// Save a project holding one MIDI track with `desc`, reload it, return the
// reloaded descriptor.
static InstrumentDesc RoundTrip(const InstrumentDesc& desc) {
    Project a;
    CommandStack stack;
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Keys"), a);
    a.Tracks().front().instrument = desc;
    ProjectIO::Save(a, kPath);

    Project b;
    ProjectIO::Load(b, kPath);
    return b.Tracks().empty() ? InstrumentDesc{} : b.Tracks().front().instrument;
}

int main() {
    // ---- the synth voice round-trips unchanged --------------------------
    {
        InstrumentDesc d;
        d.type = InstrumentType::Synth;
        d.synth.waveform = 2;
        d.synth.attack = 0.01f; d.synth.decay = 0.2f;
        d.synth.sustain = 0.5f; d.synth.release = 0.3f;
        const InstrumentDesc r = RoundTrip(d);
        CHECK(r.type == InstrumentType::Synth);
        CHECK(r.synth.waveform == 2);
        CHECK(std::abs(r.synth.decay - 0.2f) < 1e-4f);
        CHECK(std::abs(r.synth.sustain - 0.5f) < 1e-4f);
        CHECK(r.path.empty());
        // A synth track writes NO soundfont line at all.
        CHECK(!Contains(ReadFile(kPath), "soundfont"));
        // ...but always writes the instrument line, for older builds.
        CHECK(Contains(ReadFile(kPath), "instrument "));
    }

    // ---- an SFZ voice round-trips --------------------------------------
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/home/marc/Soundfonts/Brush/kit.sfz";
        d.synth.waveform = 3;      // carried along, so switching back restores it
        const InstrumentDesc r = RoundTrip(d);
        CHECK(r.type == InstrumentType::Sfz);
        CHECK(r.path == "/home/marc/Soundfonts/Brush/kit.sfz");
        CHECK(r.synth.waveform == 3);
        CHECK(Contains(ReadFile(kPath), "soundfont 1 0 "));
    }

    // ---- an SF2 voice keeps its preset index ---------------------------
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sf2;
        d.path = "/usr/share/sounds/GM.sf2";
        d.sf2Preset = 42;
        const InstrumentDesc r = RoundTrip(d);
        CHECK(r.type == InstrumentType::Sf2);
        CHECK(r.sf2Preset == 42);
        CHECK(r.path == "/usr/share/sounds/GM.sf2");
    }

    // ---- paths with spaces and quotes survive ---------------------------
    // Real libraries are full of these ("Brush jazz Samples/...").
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/home/marc/Sound fonts/Pettinhouse_Brush \"jazz\" kit.sfz";
        const InstrumentDesc r = RoundTrip(d);
        CHECK(r.type == InstrumentType::Sfz);
        CHECK(r.path == d.path);
    }

    // ---- an OLD project (no soundfont line) loads as the synth voice ----
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/x/kit.sfz";
        d.synth.waveform = 2;
        // Drop the soundfont line entirely, as a pre-soundfont build would.
        const InstrumentDesc r = LoadPatched(d, "soundfont 1 0 \"/x/kit.sfz\"\n", "");
        CHECK(r.type == InstrumentType::Synth);
        CHECK(r.synth.waveform == 2);      // the instrument line still applied
        CHECK(r.path.empty());
    }

    // ---- a type from a NEWER build clamps back to the synth -------------
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/x/kit.sfz";
        const InstrumentDesc r = LoadPatched(d, "soundfont 1 0", "soundfont 99 0");
        CHECK(r.type == InstrumentType::Synth);
    }

    // ---- a soundfont line with no path can never load: stay on the synth -
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/x/kit.sfz";
        const InstrumentDesc r = LoadPatched(d, "\"/x/kit.sfz\"", "\"\"");
        CHECK(r.type == InstrumentType::Synth);
    }

    // ---- a negative preset index is clamped -----------------------------
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sf2;
        d.path = "/x/y.sf2";
        d.sf2Preset = 5;
        const InstrumentDesc r = LoadPatched(d, "soundfont 2 5", "soundfont 2 -5");
        CHECK(r.sf2Preset == 0);
        CHECK(r.type == InstrumentType::Sf2);
    }

    // ---- a MISSING soundfont falls back to the synth, it does not crash --
    // This is the moved-sample-folder case: the project must still open and
    // the track must still play.
    {
        InstrumentDesc d;
        d.type = InstrumentType::Sfz;
        d.path = "/definitely/not/here.sfz";
        d.synth.waveform = 1;
        // Nothing was loaded into the cache, so the factory has nothing to find.
        std::unique_ptr<IInstrument> inst = MakeInstrument(d, 48000.0);
        CHECK(inst != nullptr);
        CHECK(std::strcmp(inst->Name(), "Synth") == 0);

        // And loading it really does fail, with a reason to show the user.
        std::string err;
        CHECK(SoundfontCache::Instance().Load(d.path, 0, &err) == nullptr);
        CHECK(!err.empty());
    }

    // ---- the factory builds a Sampler once the cache has the file -------
    // (covered end-to-end by sfz_parser_tests / sampler_tests; here we only
    // check the factory picks the sampler over the synth.)
    {
        InstrumentDesc d;
        d.type = InstrumentType::Synth;
        std::unique_ptr<IInstrument> inst = MakeInstrument(d, 48000.0);
        CHECK(inst != nullptr);
        CHECK(std::strcmp(inst->Name(), "Synth") == 0);
    }

    // ---- SetInstrumentCommand coalescing --------------------------------
    {
        Project p;
        CommandStack stack;
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "K"), p);
        const TrackId tid = p.Tracks().front().id;

        // Two slider tweaks of the SAME voice coalesce into one undo step.
        InstrumentDesc a; a.synth.waveform = 1;
        InstrumentDesc b; b.synth.waveform = 3;
        stack.Execute(std::make_unique<SetInstrumentCommand>(tid, a), p);
        stack.Execute(std::make_unique<SetInstrumentCommand>(tid, b), p);
        CHECK(p.FindTrack(tid)->instrument.synth.waveform == 3);
        CHECK(stack.Undo(p));
        CHECK(p.FindTrack(tid)->instrument.synth.waveform == 0);   // back to default

        // But swapping the voice KIND is its own step: a user who loads a
        // soundfont and then tweaks it expects two undos, not one.
        stack.Execute(std::make_unique<SetInstrumentCommand>(tid, a), p);
        InstrumentDesc sfz; sfz.type = InstrumentType::Sfz; sfz.path = "/k.sfz";
        stack.Execute(std::make_unique<SetInstrumentCommand>(tid, sfz), p);
        CHECK(p.FindTrack(tid)->instrument.type == InstrumentType::Sfz);
        CHECK(stack.Undo(p));
        CHECK(p.FindTrack(tid)->instrument.type == InstrumentType::Synth);
        CHECK(p.FindTrack(tid)->instrument.synth.waveform == 1);   // the tweak survives
    }

    ::unlink(kPath);
    std::printf("instrument_io_tests: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
