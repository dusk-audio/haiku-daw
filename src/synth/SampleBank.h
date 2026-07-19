// SampleBank — the loaded, immutable form of a soundfont instrument.
//
// A LoadedInstrument is what the Sampler renders from: a flat list of Regions
// (key/velocity zones) indexing into a pool of decoded SampleData. It is built
// off the realtime thread (SFZ parse or SF2 convert + WAV decode), never
// mutated afterwards, and handed around as a shared_ptr<const> so the RT
// callback can hold one without locking.
//
// SoundfontCache keys loaded instruments by (path, sf2Preset). This is not an
// optimisation: loop-record restarts rebuild every engine Bus at the loop seam,
// so a per-rebuild decode of a few-hundred-MB drum kit would stall the audio
// thread every time around the loop. The UI thread loads; the engine only ever
// looks up.
//
// Kit-free (std C++ only), host-testable.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace daw {

// One decoded sample, kept at its source channel count (unlike WavSource,
// which always widens to stereo — a sampler loading hundreds of mono drum
// hits would otherwise pay double the memory and lose the true mono signal).
struct SampleData {
    std::vector<float> data;              // interleaved, `channels` wide
    int                channels   = 1;
    double             sampleRate = 44100.0;
    int64_t            frames     = 0;
    std::string        name;              // leaf filename / SF2 sample name
};

// How a region's sample repeats. Values follow the SFZ loop_mode opcode.
enum class LoopMode {
    NoLoop = 0,       // play to the end, cut at note-off + release
    OneShot,          // play to the end, IGNORE note-off (drum hits)
    LoopContinuous,   // wrap in [loopStart, loopEnd] forever
    LoopSustain,      // wrap while held, then play out to the end on release
};

// One key/velocity zone. All fields carry SFZ semantics and units; the SF2
// converter maps its generators onto the same struct.
//
// Every region whose key AND velocity range contains a note sounds — SFZ layers
// overlapping regions rather than picking one. The Pettinghouse kits rely on
// this: their velocity zones deliberately overlap so adjacent layers crossfade.
struct Region {
    int     sampleIndex     = -1;      // into LoadedInstrument::samples

    int     loKey           = 0;       // 0..127
    int     hiKey           = 127;
    int     loVel           = 0;       // 0..127
    int     hiVel           = 127;

    int     pitchKeycenter  = 60;
    float   pitchKeytrack   = 100.0f;  // cents per key (0 = unpitched, drums)
    float   tuneCents       = 0.0f;    // tune + transpose, folded together

    float   volumeDb        = 0.0f;
    float   pan             = 0.0f;    // -100 (hard L) .. +100 (hard R)
    float   ampVeltrack     = 100.0f;  // percent

    int64_t offset          = 0;       // first sample frame to play
    int64_t end             = -1;      // last sample frame (-1 = sample end)

    LoopMode loopMode       = LoopMode::NoLoop;
    int64_t  loopStart      = 0;
    int64_t  loopEnd        = -1;      // -1 = sample end

    // Amplitude envelope, seconds (sustain is a 0..100 percent level).
    float   ampegDelay      = 0.0f;
    float   ampegAttack     = 0.0f;
    float   ampegHold       = 0.0f;
    float   ampegDecay      = 0.0f;
    float   ampegSustain    = 100.0f;
    float   ampegRelease    = 0.001f;

    // Choke groups: a note triggering a region with group == N silences any
    // sounding region whose offBy == N. SF2's exclusiveClass maps onto this;
    // it is what stops an open hi-hat when the closed one is struck.
    int     group           = 0;       // 0 = none
    int     offBy           = 0;       // 0 = none

    float   delaySec        = 0.0f;    // wait this long after note-on

    // Random round-robin (SFZ lorand/hirand): each note draws one value in
    // [0,1) and only regions whose window contains it sound. Libraries use this
    // to pick between recorded takes of the same hit. Default 0..1 = always.
    // WITHOUT this, every take of a hit layers at once — several times too loud
    // and flammy, which is how the Swirly Drums kit behaves on a player that
    // ignores the opcodes.
    float   loRand          = 0.0f;
    float   hiRand          = 1.0f;

    // Sequential round-robin (SFZ seq_length/seq_position, 1-based): the Nth
    // consecutive hit of a key selects seqPosition == ((N-1) % seqLength) + 1.
    int     seqLength       = 1;
    int     seqPosition     = 1;

    bool MatchesKey(int pitch) const { return pitch >= loKey && pitch <= hiKey; }
    bool MatchesVel(int vel) const { return vel >= loVel && vel <= hiVel; }
    // A default 0..1 window must accept every draw, including exactly 1.0f,
    // so test the full-range case explicitly rather than relying on x < hiRand.
    bool MatchesRand(float x) const {
        return (loRand <= 0.0f && hiRand >= 1.0f) || (x >= loRand && x < hiRand);
    }
};

// An instrument ready to render: regions + the samples they index.
// Immutable once built.
struct LoadedInstrument {
    std::vector<Region>     regions;
    std::vector<SampleData> samples;
    std::string             name;    // display name (leaf file / SF2 preset)

    // Longest sounding span any region can produce, in SOURCE sample frames
    // scaled to the slowest playback ratio. Used to bound how far past a note
    // the sampler must keep looking; computed once at load.
    double                  maxTailSeconds = 0.0;

    // Set at load if ANY region uses that form of round-robin, so the sampler
    // can skip the per-note work on the kits (most of them) that don't.
    bool                    hasRandom = false;
    bool                    hasSeq    = false;

    // Non-empty when the instrument loaded but is INCOMPLETE (samples dropped
    // against the memory budget). Stored here rather than only returned from
    // the loader so a cache HIT still reports it — otherwise reopening the
    // editor on a truncated kit shows a clean bill of health.
    std::string             warning;
};



using LoadedInstrumentPtr = std::shared_ptr<const LoadedInstrument>;

// Decode a WAV file wholly into memory, preserving its source channel count
// and sample rate. Reuses WavSource's header parse and sample conversion but
// not its always-stereo streaming output. Returns false (and leaves *out
// untouched) if the file is missing or unsupported.
// Ceiling on total decoded sample memory for one instrument, in BYTES of
// float sample data summed across every sample.
//
// Two reasons this exists. Soundfonts are untrusted input: an SF2 whose shdr
// table declares a thousand records all spanning the same chunk, or an SFZ
// naming a thousand large WAVs, decodes to many GB and aborts on bad_alloc.
// And v1 preloads everything into RAM, so even a legitimate multi-mic library
// can exceed the machine — Swirly Drums decodes to 2.3 GB. Hitting the ceiling
// truncates with a clear message instead of dying; the regions already loaded
// still play.
//
// Counted in bytes, not frames, because a stereo sample costs twice a mono one
// of the same length. 1 GB is far above any normal instrument (a full GM bank
// is ~30 MB, a big drum kit ~50 MB).
constexpr int64_t kMaxInstrumentBytes = 1024ll * 1024 * 1024;

// `maxBytes` caps the decode: a file whose declared size exceeds it is refused
// outright rather than read in and rejected afterwards, so a caller tracking a
// running budget never has to hold budget + one whole sample at once.
bool LoadWavToMemory(const std::string& path, SampleData* out,
                     int64_t maxBytes = kMaxInstrumentBytes);

// Bytes `path` would occupy once decoded to float, WITHOUT decoding it (the
// WAV header alone is read). 0 if the file is missing or unreadable.
//
// Lets a caller tracking a running budget tell "this sample does not fit" apart
// from "this sample is not there" — LoadWavToMemory returns false for both, and
// conflating them turns a truncated instrument into a silently missing one.
int64_t WavDecodedBytes(const std::string& path);

// Fill in maxTailSeconds and clamp region ranges into valid bounds. Called by
// each loader after it has populated regions + samples.
void FinalizeInstrument(LoadedInstrument* inst);

// Load a .sfz or .sf2 (by preset index) into a LoadedInstrument. Slow: decodes
// every referenced sample. Off-RT only. On failure returns null and, if
// `error` is non-null, sets it to a human-readable reason.
// `warning` (optional) receives a partial-load note when the instrument is
// usable but incomplete (samples dropped against the memory budget). Callers
// should surface it: the alternative is a silently half-loaded kit.
LoadedInstrumentPtr LoadSoundfont(const std::string& path, int sf2Preset,
                                  std::string* error,
                                  std::string* warning = nullptr);

// Process-wide cache of loaded instruments, keyed by (path, preset).
//
// Entries are held by shared_ptr and never mutated, so a caller that has taken
// a pointer keeps a valid instrument even if the cache is later cleared. The
// mutex guards only the map — Get/Load are called from the UI and loader
// threads, never from the audio callback.
class SoundfontCache {
public:
    // Look up an already-loaded instrument. Null if not loaded. Cheap; safe to
    // call from Engine::Load / Exporter, which must never decode.
    LoadedInstrumentPtr Get(const std::string& path, int sf2Preset) const;

    // Look up, or load and cache on a miss. SLOW on a miss — decodes the whole
    // instrument. Never call from the RT thread.
    LoadedInstrumentPtr Load(const std::string& path, int sf2Preset,
                             std::string* error, std::string* warning = nullptr);

    void Clear();

    // The process-wide instance shared by the UI, the engine and the exporter.
    static SoundfontCache& Instance();

private:
    using Key = std::pair<std::string, int>;
    mutable std::mutex                 fLock;
    std::map<Key, LoadedInstrumentPtr> fEntries;
};

} // namespace daw
