#include "SampleBank.h"

#include "SfzParser.h"
#include "Sf2ToRegions.h"
#include "../engine/WavSource.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace daw {

bool LoadWavToMemory(const std::string& path, SampleData* out,
                     int64_t maxBytes) {
    if (!out) return false;
    if (maxBytes <= 0) return false;

    WavSource src;
    if (!src.Open(path))
        return false;

    SampleData sd;
    sd.channels   = src.SourceChannels();
    sd.sampleRate = src.FrameRate();
    sd.frames     = src.TotalFrames();
    if (sd.channels < 1 || sd.sampleRate <= 0.0 || sd.frames <= 0)
        return false;

    // Refuse before decoding when the file cannot fit the caller's budget.
    // WavSource::Open has already clamped TotalFrames to the bytes the file
    // really holds, so this is a truthful size, not a declared one.
    if (sd.frames * (int64_t)sd.channels * (int64_t)sizeof(float) > maxBytes)
        return false;

    // TotalFrames comes from the DECLARED data-chunk size, which is untrusted:
    // a 48-byte WAV can claim a 4 GB data chunk and this reserve would ask for
    // 17 GB, throwing std::bad_alloc — uncaught, so std::terminate. Reserve
    // only what the instrument budget allows and let the insert loop below,
    // which is bounded by bytes actually read, grow the vector if the file is
    // genuinely that large.
    const int64_t declared = sd.frames * (int64_t)sd.channels;
    if (declared > 0 && declared * (int64_t)sizeof(float) <= maxBytes)
        sd.data.reserve((size_t)declared);

    const float*  blk = nullptr;
    size_t        n   = 0;
    const size_t  cap = (size_t)(maxBytes / (int64_t)sizeof(float));
    while (src.ReadChunkNative(&blk, &n)) {
        if (sd.data.size() >= cap) break;   // absurd file: stop, don't OOM
        // Clamp the chunk itself: testing only before the insert let the last
        // one overshoot the cap by a whole block.
        size_t take = n * (size_t)sd.channels;
        const size_t room = cap - sd.data.size();
        if (take > room) take = room;
        sd.data.insert(sd.data.end(), blk, blk + take);
    }

    sd.frames = (int64_t)(sd.data.size() / (size_t)sd.channels);
    if (sd.frames <= 0)
        return false;

    // Leaf name, for display and error messages.
    const size_t slash = path.find_last_of("/\\");
    sd.name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    *out = std::move(sd);
    return true;
}

int64_t WavDecodedBytes(const std::string& path) {
    WavSource src;
    if (!src.Open(path)) return 0;
    const int64_t frames = src.TotalFrames();
    const int64_t ch     = src.SourceChannels();
    if (frames <= 0 || ch <= 0) return 0;
    return frames * ch * (int64_t)sizeof(float);
}

void FinalizeInstrument(LoadedInstrument* inst) {
    if (!inst) return;

    // Drop regions pointing at a sample that failed to load or is empty, so
    // the sampler never has to bounds-check sampleIndex on the RT thread.
    const size_t nSamples = inst->samples.size();
    inst->regions.erase(
        std::remove_if(inst->regions.begin(), inst->regions.end(),
                       [&](const Region& r) {
                           return r.sampleIndex < 0
                               || (size_t)r.sampleIndex >= nSamples
                               || inst->samples[(size_t)r.sampleIndex].frames <= 0;
                       }),
        inst->regions.end());

    double maxTail = 0.0;
    for (Region& r : inst->regions) {
        const SampleData& s = inst->samples[(size_t)r.sampleIndex];

        // Clamp every span into the sample. These come from untrusted files:
        // an SFZ can name any end=/loop_end=, and SF2 shdr offsets are
        // file-declared. Everything downstream indexes without re-checking.
        if (r.offset < 0) r.offset = 0;
        if (r.offset > s.frames - 1) r.offset = s.frames - 1;
        if (r.end < 0 || r.end > s.frames - 1) r.end = s.frames - 1;
        if (r.end < r.offset) r.end = r.offset;

        if (r.loopEnd < 0 || r.loopEnd > r.end) r.loopEnd = r.end;
        if (r.loopStart < r.offset) r.loopStart = r.offset;
        if (r.loopStart > r.loopEnd) r.loopStart = r.loopEnd;
        // A degenerate loop (start == end) would spin on one sample forever.
        if (r.loopMode != LoopMode::NoLoop && r.loopMode != LoopMode::OneShot
            && r.loopEnd <= r.loopStart)
            r.loopMode = LoopMode::NoLoop;

        r.loKey = std::clamp(r.loKey, 0, 127);
        r.hiKey = std::clamp(r.hiKey, 0, 127);
        r.loVel = std::clamp(r.loVel, 0, 127);
        r.hiVel = std::clamp(r.hiVel, 0, 127);
        r.pitchKeycenter = std::clamp(r.pitchKeycenter, 0, 127);

        // Clamp the floats from BOTH sides. Only the lower bound used to be
        // checked, which left three ways for a malformed file to reach the
        // audio thread:
        //   - volume=10000 -> pow(10, 500) = +inf, and the sampler writes Inf
        //     into the node buffer. The engine's isfinite guard is on the
        //     MASTER output, after the track FX chain, so a reverb or delay
        //     ingests the Inf first and stays poisoned for the rest of the
        //     session; the guard then zeroes the whole block, silencing the
        //     entire mix rather than one voice.
        //   - unbounded envelope/delay times and tune scale into durations that
        //     overflow the double -> Frame conversion (undefined behaviour).
        //   - pitch_keytrack/tune drive the playback ratio, where a huge
        //     negative value yields a denormal ratio and a colossal duration.
        // kMaxSeconds is an hour: longer than any envelope stage is musically.
        constexpr float kMaxSeconds = 3600.0f;
        r.volumeDb     = std::clamp(r.volumeDb, -144.0f, 24.0f);
        r.ampegDelay   = std::clamp(r.ampegDelay,   0.0f, kMaxSeconds);
        r.ampegAttack  = std::clamp(r.ampegAttack,  0.0f, kMaxSeconds);
        r.ampegHold    = std::clamp(r.ampegHold,    0.0f, kMaxSeconds);
        r.ampegDecay   = std::clamp(r.ampegDecay,   0.0f, kMaxSeconds);
        r.ampegRelease = std::clamp(r.ampegRelease, 0.0f, kMaxSeconds);
        r.ampegSustain = std::clamp(r.ampegSustain, 0.0f, 100.0f);
        r.delaySec     = std::clamp(r.delaySec,     0.0f, kMaxSeconds);
        r.pan          = std::clamp(r.pan,       -100.0f, 100.0f);
        r.ampVeltrack  = std::clamp(r.ampVeltrack, -100.0f, 100.0f);
        // +/- 10 octaves of key tracking and +/- 100 semitones of tune is far
        // past anything musical and keeps the playback ratio in a sane range.
        r.pitchKeytrack = std::clamp(r.pitchKeytrack, -1200.0f, 1200.0f);
        r.tuneCents     = std::clamp(r.tuneCents, -10000.0f, 10000.0f);
        // NaN survives std::clamp (both comparisons fail), so reject it here.
        if (!std::isfinite(r.volumeDb))      r.volumeDb      = 0.0f;
        if (!std::isfinite(r.pan))           r.pan           = 0.0f;
        if (!std::isfinite(r.pitchKeytrack)) r.pitchKeytrack = 100.0f;
        if (!std::isfinite(r.tuneCents))     r.tuneCents     = 0.0f;
        if (!std::isfinite(r.ampVeltrack))   r.ampVeltrack   = 100.0f;
        // NaN passes std::clamp above (both comparisons are false), and these
        // all feed frame arithmetic and the envelope shape. Zero is the
        // no-op duration for every one of them.
        if (!std::isfinite(r.ampegDelay))   r.ampegDelay   = 0.0f;
        if (!std::isfinite(r.ampegAttack))  r.ampegAttack  = 0.0f;
        if (!std::isfinite(r.ampegHold))    r.ampegHold    = 0.0f;
        if (!std::isfinite(r.ampegDecay))   r.ampegDecay   = 0.0f;
        if (!std::isfinite(r.ampegRelease)) r.ampegRelease = 0.0f;
        if (!std::isfinite(r.ampegSustain)) r.ampegSustain = 100.0f;
        if (!std::isfinite(r.delaySec))     r.delaySec     = 0.0f;

        // Round-robin windows.
        if (!std::isfinite(r.loRand)) r.loRand = 0.0f;
        if (!std::isfinite(r.hiRand)) r.hiRand = 1.0f;
        r.loRand = std::clamp(r.loRand, 0.0f, 1.0f);
        r.hiRand = std::clamp(r.hiRand, 0.0f, 1.0f);
        if (r.hiRand < r.loRand) r.hiRand = r.loRand;
        r.seqLength   = std::clamp(r.seqLength, 1, 128);
        r.seqPosition = std::clamp(r.seqPosition, 1, r.seqLength);
        if (r.loRand > 0.0f || r.hiRand < 1.0f) inst->hasRandom = true;
        if (r.seqLength > 1)                    inst->hasSeq    = true;

        // Worst-case sounding length past note-on, in seconds and independent
        // of the output rate: the sample played at its SLOWEST pitch, plus the
        // envelope tail. A looping region's length is governed by how long the
        // note is held, so it contributes only its release.
        //
        // The slowest pitch is the bottom of the key range only when keytrack
        // is POSITIVE; a negative keytrack (rare, but legal, and produced by an
        // SF2 with a negative scaleTuning) inverts that, so the top of the
        // range is the slow end. Picking the wrong one under-estimates the tail.
        const int slowKey = (r.pitchKeytrack < 0.0f) ? r.hiKey : r.loKey;
        const double cents = (double)(slowKey - r.pitchKeycenter) * r.pitchKeytrack
                             + r.tuneCents;
        const double pitchRatio = std::pow(2.0, cents / 1200.0);
        // BOTH delays push the voice's start later, so both extend how far past
        // the note it can still be sounding. GatherVoices folds `delay` and
        // `ampeg_delay` together into the voice start, and rejects a note whose
        // start + length + tail is already behind the block — omitting
        // ampegDelay here made that early-out fire before the delayed voice was
        // due, so a region carrying ampeg_delay (or an SF2 delayVolEnv) never
        // sounded at all.
        double tail = (double)r.delaySec + (double)r.ampegDelay
                    + (double)r.ampegRelease;
        // LoopSustain leaves the loop at note-off and plays out the rest of the
        // sample, so it needs the source-duration term too — only a truly
        // endless loop (LoopContinuous) is bounded by the release alone.
        if (r.loopMode != LoopMode::LoopContinuous) {
            const double srcSeconds = (double)(r.end - r.offset + 1) / s.sampleRate;
            tail += srcSeconds / std::max(1e-6, pitchRatio);
        }
        if (tail > maxTail) maxTail = tail;
    }
    inst->maxTailSeconds = maxTail;
}

namespace {
// Case-insensitive extension test on a path.
bool HasExt(const std::string& path, const char* ext) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = path.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e == ext;
}
} // namespace

LoadedInstrumentPtr LoadSoundfont(const std::string& path, int sf2Preset,
                                  std::string* error, std::string* warning) {
    auto fail = [&](const char* why) -> LoadedInstrumentPtr {
        if (error) *error = why;
        return nullptr;
    };

    if (path.empty())
        return fail("no soundfont file set");

    // A loader may succeed PARTIALLY (samples dropped against the memory
    // budget). That is not a failure — the instrument plays — but it must not
    // be swallowed either, or a half-loaded kit looks fully loaded.
    if (HasExt(path, "sfz")) {
        auto inst = std::make_shared<LoadedInstrument>();
        std::string err, warn;
        if (!LoadSfz(path, inst.get(), &err, &warn))
            return fail(err.empty() ? "could not load SFZ" : err.c_str());
        // ParseSfzText returns true but fills `err` when it parsed fine and
        // still produced nothing — e.g. every sample file was missing. That is
        // the message that actually tells the user what to fix, so prefer it
        // over the generic one rather than overwriting it.
        if (inst->regions.empty())
            return fail(err.empty() ? "SFZ has no playable regions" : err.c_str());
        inst->warning = warn;
        if (warning) *warning = warn;
        return inst;
    }

    if (HasExt(path, "sf2")) {
        auto inst = std::make_shared<LoadedInstrument>();
        std::string err, warn;
        if (!LoadSf2Preset(path, sf2Preset, inst.get(), &err, &warn))
            return fail(err.empty() ? "could not load SF2" : err.c_str());
        inst->warning = warn;
        if (warning) *warning = warn;
        return inst;
    }

    return fail("not a .sfz or .sf2 file");
}

LoadedInstrumentPtr SoundfontCache::Get(const std::string& path,
                                        int sf2Preset) const {
    std::lock_guard<std::mutex> g(fLock);
    auto it = fEntries.find(Key(path, sf2Preset));
    return it == fEntries.end() ? nullptr : it->second;
}

LoadedInstrumentPtr SoundfontCache::Load(const std::string& path, int sf2Preset,
                                         std::string* error,
                                         std::string* warning) {
    if (auto hit = Get(path, sf2Preset)) {
        if (warning) *warning = hit->warning;   // a hit is still partial
        return hit;
    }

    // Decode OUTSIDE the lock: loading a large kit takes seconds, and holding
    // the lock would block the engine's Get() calls (and every other track's
    // load) for the whole decode. A concurrent duplicate load just wastes work
    // once; the insert below keeps whichever landed first, so every caller
    // still ends up sharing one instrument.
    std::string err;
    LoadedInstrumentPtr inst = LoadSoundfont(path, sf2Preset, &err, warning);
    if (!inst) {
        if (error) *error = err;
        return nullptr;
    }

    std::lock_guard<std::mutex> g(fLock);
    auto ins = fEntries.emplace(Key(path, sf2Preset), inst);
    return ins.first->second;
}

void SoundfontCache::Clear() {
    std::lock_guard<std::mutex> g(fLock);
    fEntries.clear();
}

SoundfontCache& SoundfontCache::Instance() {
    static SoundfontCache cache;
    return cache;
}

} // namespace daw
