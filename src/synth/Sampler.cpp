#include "Sampler.h"

#include "../model/Project.h"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

// How long a choked voice takes to fade out. SFZ's off_mode default is a fast
// fade rather than a hard cut, which is what stops a hi-hat choke clicking.
constexpr double kChokeSeconds = 0.006;

// SFZ amp_veltrack: 0 = velocity does not affect level, 100 = level follows
// velocity squared (the format's default). Negative values invert it.
float VelocityGain(int velocity, float veltrackPercent) {
    const float vn = std::clamp(velocity / 127.0f, 0.0f, 1.0f);
    const float g  = 1.0f - (veltrackPercent / 100.0f) * (1.0f - vn * vn);
    return g > 0.0f ? g : 0.0f;
}

// SFZ ampeg, evaluated at `t` seconds since the voice started (its delay
// already consumed by the caller). `held` is how long the note is held; past
// that the release tail runs from whatever level the envelope had reached.
double AmpEnv(double t, double held, const Region& r, bool ignoreNoteOff) {
    const double sus = r.ampegSustain / 100.0;
    auto ahds = [&](double x) -> double {
        if (r.ampegAttack > 0.0f) {
            if (x < r.ampegAttack) return x / r.ampegAttack;
            x -= r.ampegAttack;
        }
        if (x < r.ampegHold) return 1.0;
        x -= r.ampegHold;
        if (r.ampegDecay > 0.0f) {
            if (x < r.ampegDecay) return 1.0 - (1.0 - sus) * (x / r.ampegDecay);
            return sus;
        }
        return sus;
    };
    // one_shot ignores note-off: the sample plays out in full. This is what
    // makes drum kits behave, where note lengths are meaningless.
    if (ignoreNoteOff || t < held) return ahds(t);
    if (r.ampegRelease <= 0.0f) return 0.0;
    const double u = (t - held) / r.ampegRelease;
    if (u >= 1.0) return 0.0;
    return ahds(held) * (1.0 - u);
}

// A deterministic pseudo-random value in [0,1) for one note.
//
// SFZ lorand/hirand picks ONE take per note; every region of that note must see
// the same draw. Seeding from the note itself (rather than a running counter)
// keeps the sampler stateless: the same note yields the same take no matter
// which block it is rendered in, so a bounce matches playback and a seek does
// not reshuffle the kit. splitmix64's finaliser, which mixes well enough that
// adjacent frames/pitches don't correlate audibly.
float NoteRandom(Frame startFrame, int pitch) {
    uint64_t x = (uint64_t)startFrame * 0x9E3779B97F4A7C15ull
               + (uint64_t)(uint32_t)pitch * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    // Top 24 bits -> [0,1). Never reaches 1.0f.
    return (float)(x >> 40) * (1.0f / 16777216.0f);
}

// Seconds/frames as a double -> Frame, safely. Converting an out-of-range
// double to an integer type is undefined behaviour, and every duration here is
// scaled from a file-supplied value (delay, tune, envelope times). Clamp to a
// century of frames, which is longer than any timeline and cannot overflow when
// added to a note position.
Frame ToFrames(double v) {
    constexpr double kLimit = 48000.0 * 60.0 * 60.0 * 24.0 * 365.0 * 100.0;
    if (!std::isfinite(v) || v <= 0.0) return 0;
    return (Frame)(v > kLimit ? kLimit : v);
}

// Equal-power pan for an SFZ pan value (-100 hard left .. +100 hard right).
// Same curve as Engine::EqualPowerGains, so a region panned centre sits where
// a track panned centre does.
void PanGains(float pan, float* l, float* r) {
    const float p = std::clamp(pan / 100.0f, -1.0f, 1.0f);
    const float theta = (p * 0.5f + 0.5f) * (float)M_PI * 0.5f;
    *l = std::cos(theta);
    *r = std::sin(theta);
}

} // namespace

Sampler::Sampler(LoadedInstrumentPtr inst, double sampleRate)
    : fInst(std::move(inst)), fSampleRate(sampleRate > 0 ? sampleRate : 48000.0) {
    // AddVoice never grows past kMaxVoices, so this reserve makes Render
    // allocation-free for the whole life of the object.
    fVoices.reserve(kMaxVoices);
}

void Sampler::Prepare(double sampleRate) {
    if (sampleRate > 0) fSampleRate = sampleRate;
    fVoices.reserve(kMaxVoices);   // no-op after the first call
}

int Sampler::SeqIndex(const std::vector<MidiNote>& notes, int pitch,
                      Frame before, int orderBefore) const {
    // Count earlier hits of the same key. Ties on start frame break on the
    // note's index so the count is well defined even for stacked notes, and so
    // it does not depend on the block being rendered.
    int n = 0;
    int i = 0;
    for (const MidiNote& m : notes) {
        const int mo = i++;
        if (m.pitch != pitch) continue;
        if (m.startFrame < before || (m.startFrame == before && mo < orderBefore))
            n++;
    }
    return n;
}

void Sampler::AddVoice(const Voice& v) {
    if (fVoices.size() < (size_t)kMaxVoices) {
        fVoices.push_back(v);       // within the reserve: never allocates
        return;
    }
    // Full: evict the oldest-started voice, but only if this one is newer, so
    // the surviving set is the most recent kMaxVoices of the candidates. Linear
    // scan is fine — it only runs on content that exceeds the cap at all.
    size_t oldest = 0;
    for (size_t i = 1; i < fVoices.size(); i++) {
        const Voice& o = fVoices[i];
        const Voice& b = fVoices[oldest];
        if (o.start < b.start || (o.start == b.start && o.order < b.order))
            oldest = i;
    }
    const Voice& o = fVoices[oldest];
    if (v.start > o.start || (v.start == o.start && v.order > o.order))
        fVoices[oldest] = v;
}

Frame Sampler::ChokeTime(const std::vector<MidiNote>& notes, int group,
                         Frame after) const {
    Frame best = -1;
    for (const MidiNote& n : notes) {
        if (n.startFrame <= after) continue;
        if (best >= 0 && n.startFrame >= best) continue;
        for (const Region& r : fInst->regions) {
            if (r.group != group) continue;
            if (!r.MatchesKey(n.pitch) || !r.MatchesVel(n.velocity)) continue;
            best = n.startFrame;
            break;
        }
    }
    return best;
}

void Sampler::GatherVoices(const std::vector<MidiNote>& notes, Frame blockStart,
                           Frame blockEnd) {
    fVoices.clear();

    const double sr = fSampleRate;

    // Longest a voice can sound past its note, so a note that cannot possibly
    // overlap this block is rejected in O(1) instead of O(regions).
    //
    // Without this every block scanned notes x regions — a 2000-note track on a
    // 200-region kit is 400k range tests per 10 ms block, essentially all of
    // them discarded. This does NOT remove the per-block scan of the note list
    // itself; doing that properly means building note/region indexes off the
    // realtime thread, which needs an IInstrument signature change (the sampler
    // is handed the notes per call and does not own them) and is left alone
    // here deliberately.
    const Frame tailFrames = ToFrames(fInst->maxTailSeconds * sr) + 1;

    int order = 0;
    for (const MidiNote& n : notes) {
        const int noteOrder = order++;

        // Cheap window reject. A region's own delay is already folded into
        // maxTailSeconds, so a voice can never start before its note nor end
        // later than note + length + tail.
        if (n.startFrame >= blockEnd) continue;
        if (n.startFrame + n.lengthFrames + tailFrames <= blockStart) continue;

        // One random draw per NOTE, shared by all its regions, so lorand/hirand
        // selects a single take instead of layering every take at once.
        const float rnd = fInst->hasRandom ? NoteRandom(n.startFrame, n.pitch)
                                           : 0.0f;
        // Sequence counter, only for kits that actually use seq_length (it
        // costs a scan of the note list).
        const int seqIdx = fInst->hasSeq
            ? SeqIndex(notes, n.pitch, n.startFrame, noteOrder) : 0;

        for (const Region& r : fInst->regions) {
            if (!r.MatchesKey(n.pitch) || !r.MatchesVel(n.velocity)) continue;
            if (fInst->hasRandom && !r.MatchesRand(rnd)) continue;
            if (fInst->hasSeq && r.seqLength > 1
                && (seqIdx % r.seqLength) + 1 != r.seqPosition) continue;

            const SampleData& s = fInst->samples[(size_t)r.sampleIndex];

            // Source frames consumed per output frame: the pitch shift, times
            // the sample-rate conversion. pitch_keytrack=0 (drum kits) makes
            // the key term vanish, so a hit plays at its recorded pitch.
            const double cents = (double)(n.pitch - r.pitchKeycenter) * r.pitchKeytrack
                                 + r.tuneCents;
            const double ratio = std::pow(2.0, cents / 1200.0) * (s.sampleRate / sr);
            if (!(ratio > 0.0) || !std::isfinite(ratio)) continue;

            // ampeg_delay and delay both push the voice later; fold them into
            // the start so the envelope below begins at its attack.
            //
            // Every double -> Frame conversion here goes through ToFrames:
            // converting an out-of-range double to an integer type is undefined
            // behaviour, and these are all scaled from file-supplied values.
            const Frame start = n.startFrame
                              + ToFrames((double)(r.delaySec + r.ampegDelay) * sr);
            const Frame off   = n.startFrame + n.lengthFrames;

            const bool oneShot = (r.loopMode == LoopMode::OneShot);
            const bool sustain = (r.loopMode == LoopMode::LoopSustain);
            const bool loops   = (r.loopMode == LoopMode::LoopContinuous
                               || sustain);

            // When the voice falls silent, in output frames.
            Frame end;
            const double srcFrames = (double)(r.end - r.offset + 1);
            const Frame  playOut   = ToFrames(srcFrames / ratio);
            const Frame  relFrames = ToFrames((double)r.ampegRelease * sr);
            if (oneShot) {
                end = start + playOut;                 // note-off is ignored
            } else if (sustain) {
                // loop_sustain loops only WHILE HELD; at note-off it leaves the
                // loop and plays out the remainder of the sample. So the tail
                // is the longer of the release and what is left after the loop
                // point — it is not just `off + release` like a plain loop.
                const double afterLoop =
                    (double)(r.end - r.loopStart + 1) / ratio;
                end = off + std::max<Frame>(relFrames, ToFrames(afterLoop));
            } else if (loops) {
                end = off + relFrames;
            } else {
                end = std::min<Frame>(start + playOut, off + relFrames);
            }
            if (end <= start) continue;

            if (end <= blockStart || start >= blockEnd) continue;

            Voice v;
            v.region = &r;
            v.start  = start;
            v.off    = off;
            v.end    = end;
            v.amp    = VelocityGain(n.velocity, r.ampVeltrack)
                     * std::pow(10.0f, r.volumeDb / 20.0f);
            v.ratio  = ratio;
            v.order  = noteOrder;

            // Choke: a later note in a group this region is off_by silences it.
            if (r.offBy != 0) {
                const Frame ct = ChokeTime(notes, r.offBy, start);
                if (ct >= 0) {
                    v.chokeAt = ct;
                    const Frame hardEnd = ct + ToFrames(kChokeSeconds * sr);
                    if (hardEnd < v.end) v.end = hardEnd;
                    if (v.end <= blockStart) continue;
                }
            }

            // Bounded insertion: fVoices never exceeds its reserve, so this
            // allocates nothing, and the cap keeps the most RECENT voices
            // rather than whichever happened to be scanned first.
            AddVoice(v);
        }
    }
}

void Sampler::Render(const std::vector<MidiNote>& notes,
                     float* out, size_t frames, Frame blockStart,
                     StereoGain from, StereoGain to) {
    if (!fInst || fInst->regions.empty() || frames == 0)
        return;

    const Frame blockEnd = blockStart + (Frame)frames;
    GatherVoices(notes, blockStart, blockEnd);
    if (fVoices.empty())
        return;

    // Linear per-sample glide from `from` to `to`, reaching `to` exactly on the
    // last frame so the next block continues where this one ended. Matches
    // Synth::Render, so switching a track's voice does not change how its
    // channel controllers de-zipper.
    const bool  ramping = (from.l != to.l) || (from.r != to.r);
    const float step    = 1.0f / (float)frames;
    const float dL      = (to.l - from.l) * step;
    const float dR      = (to.r - from.r) * step;

    const double sr        = fSampleRate;
    const double chokeSpan = kChokeSeconds * sr;

    for (const Voice& v : fVoices) {
        const Region&     r = *v.region;
        const SampleData& s = fInst->samples[(size_t)r.sampleIndex];
        const float*      d = s.data.data();
        const int         ch = s.channels;
        const bool        oneShot = (r.loopMode == LoopMode::OneShot);
        const bool        sustain = (r.loopMode == LoopMode::LoopSustain);
        const bool        loops   = (r.loopMode == LoopMode::LoopContinuous
                                  || sustain);
        const double      loopSpan = loops ? (double)(r.loopEnd - r.loopStart + 1) : 0.0;
        const double      held = (double)(v.off - v.start) / sr;
        // Frames the note is held for, used to freeze the loop at note-off.
        const double      heldFrames = (double)(v.off - v.start);

        float pl, pr;
        PanGains(r.pan, &pl, &pr);

        const Frame i0 = std::max<Frame>(blockStart, v.start);
        const Frame i1 = std::min<Frame>(blockEnd, v.end);

        for (Frame g = i0; g < i1; g++) {
            const size_t i   = (size_t)(g - blockStart);
            const double rel = (double)(g - v.start);

            double env = AmpEnv(rel / sr, held, r, oneShot);
            if (v.chokeAt >= 0 && g >= v.chokeAt) {
                const double u = (double)(g - v.chokeAt) / chokeSpan;
                env *= (u >= 1.0) ? 0.0 : (1.0 - u);
            }
            if (env <= 0.0) continue;

            // Read position in source frames. Pure arithmetic on the block's
            // global frame — this is what keeps the sampler stateless.
            double pos = (double)r.offset + rel * v.ratio;
            bool   wrapping = loops;
            if (sustain && rel > heldFrames) {
                // Past note-off: leave the loop and run forward to the sample
                // end. Where we leave from is the wrapped position AT note-off,
                // computed here rather than carried — that keeps the voice a
                // pure function of the block, so a bounce still matches
                // playback and a seek lands in the same place.
                double atOff = (double)r.offset + heldFrames * v.ratio;
                if (atOff > (double)r.loopEnd) {
                    const double over = atOff - (double)r.loopStart;
                    atOff = (double)r.loopStart + std::fmod(over, loopSpan);
                }
                pos = atOff + (rel - heldFrames) * v.ratio;
                wrapping = false;
                // `pos` only ever advances (ratio > 0), so once it is past the
                // end this voice is done for the whole block — break rather
                // than re-testing every remaining frame. A sustain voice with a
                // long release outlives its sample by design, so this is the
                // common case, not an edge one.
                if (pos > (double)r.end) break;      // played out after release
            } else if (wrapping) {
                if (pos > (double)r.loopEnd) {
                    const double over = pos - (double)r.loopStart;
                    pos = (double)r.loopStart + std::fmod(over, loopSpan);
                }
                // fmod can land in (loopEnd, loopEnd+1) — inside the loop, but
                // past r.end when loop_end defaults to the sample end. Falling
                // through to the end test below would drop that frame once per
                // loop period: a periodic click on every sustained note.
                // Interpolation wraps to loopStart via p1, so just keep it.
            } else if (pos > (double)r.end) {
                break;                               // played out (see above)
            }

            int64_t p0 = (int64_t)pos;
            if (p0 < r.offset) p0 = r.offset;
            if (p0 > r.end)    p0 = r.end;   // the loop-wrap case above
            const float frac = (float)(pos - (double)p0);
            int64_t p1 = p0 + 1;
            if (p1 > r.end) p1 = wrapping ? r.loopStart : r.end;

            const float* a = d + (size_t)p0 * (size_t)ch;
            const float* b = d + (size_t)p1 * (size_t)ch;

            const float sl = a[0] + (b[0] - a[0]) * frac;
            const float sr2 = (ch > 1) ? (a[1] + (b[1] - a[1]) * frac) : sl;

            const float amp = v.amp * (float)env;
            const float gl  = ramping ? (from.l + dL * (float)(i + 1)) : to.l;
            const float gr  = ramping ? (from.r + dR * (float)(i + 1)) : to.r;

            out[i * 2 + 0] += sl  * pl * amp * gl;
            out[i * 2 + 1] += sr2 * pr * amp * gr;
        }
    }
}

} // namespace daw
