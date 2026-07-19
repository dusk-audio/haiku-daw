#include "Sf2ToRegions.h"

#include "Sf2Reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

namespace daw {
namespace {

// A zone's generators flattened to oper -> raw amount, so a later level can
// override an earlier one by simple assignment.
using GenMap = std::map<uint16_t, uint16_t>;

void MergeZone(GenMap& into, const Sf2Zone& z) {
    for (const Sf2Generator& g : z.gens)
        into[g.oper] = g.amount;
}

bool HasGen(const Sf2Zone& z, uint16_t oper) { return z.Find(oper) != nullptr; }

// Signed read with a default. SF2 stores every genAmount as a 16-bit word;
// which of the two readings is correct depends on the operator.
int GenOr(const GenMap& m, uint16_t oper, int fallback) {
    auto it = m.find(oper);
    return it == m.end() ? fallback : (int)(int16_t)it->second;
}

bool HasKey(const GenMap& m, uint16_t oper) { return m.find(oper) != m.end(); }

// Unsigned read, for range generators (lo/hi packed as two bytes).
int GenRaw(const GenMap& m, uint16_t oper, int fallback) {
    auto it = m.find(oper);
    return it == m.end() ? fallback : (int)it->second;
}

double TimecentsToSeconds(int tc) { return std::pow(2.0, (double)tc / 1200.0); }

// Extract one mono sample [start, end) of 16-bit PCM out of the smpl chunk.
//
// shdr start/end are file-declared and UNTRUSTED: validate them against the
// smpl chunk's actual byte size before sizing any allocation. A crafted `end`
// of 0x7FFFFFFF would otherwise request a ~4 GB buffer, and frames*2 computed
// in int is signed-overflow UB. Carried over from DuskStudio verbatim, cap and
// all — this was a real hardening fix there.
bool ExtractSample(std::ifstream& in, int64_t smplOffset, int64_t smplSize,
                   const Sf2Sample& s, SampleData* out) {
    if (s.end <= s.start) return false;
    const int64_t frames64 = (int64_t)s.end - (int64_t)s.start;
    if (smplSize <= 0 || (int64_t)s.end * 2 > smplSize) return false;
    if (frames64 > 0x10000000) return false;   // 256 M frames ~ 512 MB; no real sample

    const size_t numFrames = (size_t)frames64;

    // Decode in fixed-size chunks straight into the float buffer. Reading the
    // whole PCM into a raw vector first meant holding 2 bytes AND 4 bytes per
    // frame at once — 6 bytes/frame peak, 1.5 GB at the 256 M-frame cap.
    out->data.resize(numFrames);
    in.clear();
    in.seekg(smplOffset + (int64_t)s.start * 2, std::ios::beg);

    constexpr size_t kChunkFrames = 8192;
    std::vector<uint8_t> raw(kChunkFrames * 2);
    size_t done = 0;
    while (done < numFrames) {
        const size_t want = std::min(kChunkFrames, numFrames - done);
        in.read((char*)raw.data(), (std::streamsize)(want * 2));
        if ((size_t)in.gcount() != want * 2) return false;   // short/truncated
        for (size_t i = 0; i < want; i++) {
            // SF2 PCM is little-endian signed 16-bit. Read the bytes explicitly
            // rather than reinterpret-casting, so endianness is not assumed.
            const int16_t v = (int16_t)((uint16_t)raw[i * 2]
                                      | ((uint16_t)raw[i * 2 + 1] << 8));
            out->data[done + i] = (float)v / 32768.0f;
        }
        done += want;
    }
    out->channels   = 1;
    out->sampleRate = s.sampleRate > 0 ? (double)s.sampleRate : 44100.0;
    out->frames     = (int64_t)numFrames;
    out->name       = s.name;
    return true;
}

} // namespace

std::vector<Sf2PresetInfo> ListSf2Presets(const std::string& path,
                                          std::string* error) {
    std::vector<Sf2PresetInfo> out;
    const Sf2File f = ReadSf2(path);
    if (!f.ok) {
        if (error) *error = f.error;
        return out;
    }
    out.reserve(f.presets.size());
    for (size_t i = 0; i < f.presets.size(); i++) {
        Sf2PresetInfo pi;
        pi.name    = f.presets[i].name;
        pi.index   = (int)i;
        pi.bank    = (int)f.presets[i].bank;
        pi.program = (int)f.presets[i].preset;
        out.push_back(std::move(pi));
    }
    // Program-first ordering, so the list reads like a GM bank rather than
    // like the file's internal record order.
    std::stable_sort(out.begin(), out.end(),
                     [](const Sf2PresetInfo& a, const Sf2PresetInfo& b) {
                         if (a.bank != b.bank) return a.bank < b.bank;
                         return a.program < b.program;
                     });
    return out;
}

bool LoadSf2Preset(const std::string& path, int presetIndex,
                   LoadedInstrument* out, std::string* error,
                   std::string* warning) {
    if (!out) return false;

    const Sf2File f = ReadSf2(path);
    if (!f.ok) {
        if (error) *error = f.error;
        return false;
    }
    if (f.presets.empty()) {
        if (error) *error = "SF2 has no presets";
        return false;
    }

    presetIndex = std::clamp(presetIndex, 0, (int)f.presets.size() - 1);
    const Sf2Preset& preset = f.presets[(size_t)presetIndex];
    out->name = preset.name;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "could not reopen SF2 for sample extraction";
        return false;
    }

    // Preset-global zone (preset zone[0] when it carries no instrument
    // generator) applies to every instrument the preset uses.
    GenMap presetGlobal;
    size_t firstInstZone = 0;
    if (!preset.zones.empty() && !HasGen(preset.zones[0], kGenInstrument)) {
        MergeZone(presetGlobal, preset.zones[0]);
        firstInstZone = 1;
    }

    std::map<int, int> extracted;   // SF2 sample index -> our sample index
    int64_t            totalBytes  = 0;   // against kMaxInstrumentBytes
    bool               overBudget  = false;

    for (size_t pz = firstInstZone; pz < preset.zones.size(); pz++) {
        const Sf2Zone& presetZone = preset.zones[pz];
        const Sf2Generator* instGen = presetZone.Find(kGenInstrument);
        if (!instGen) continue;
        const int instIdx = (int)instGen->amount;
        if (instIdx < 0 || instIdx >= (int)f.instruments.size()) continue;
        const Sf2Instrument& inst = f.instruments[(size_t)instIdx];

        // Per-preset-zone layer: preset-global overridden by this zone.
        GenMap presetLayer = presetGlobal;
        MergeZone(presetLayer, presetZone);

        // Instrument-global zone defaults.
        GenMap instGlobal;
        size_t firstSampleZone = 0;
        if (!inst.zones.empty() && !HasGen(inst.zones[0], kGenSampleID)) {
            MergeZone(instGlobal, inst.zones[0]);
            firstSampleZone = 1;
        }

        for (size_t iz = firstSampleZone; iz < inst.zones.size(); iz++) {
            const Sf2Zone& instZone = inst.zones[iz];
            const Sf2Generator* sidGen = instZone.Find(kGenSampleID);
            if (!sidGen) continue;
            const int sampleIdx = (int)sidGen->amount;
            if (sampleIdx < 0 || sampleIdx >= (int)f.samples.size()) continue;
            const Sf2Sample& smp = f.samples[(size_t)sampleIdx];

            // Effective instrument-level gens: global, then zone override.
            GenMap eff = instGlobal;
            MergeZone(eff, instZone);

            // Key / velocity ranges INTERSECT across the two levels.
            int loKey = 0, hiKey = 127, loVel = 0, hiVel = 127;
            if (HasKey(eff, kGenKeyRange)) {
                const int v = GenRaw(eff, kGenKeyRange, 0);
                loKey = v & 0xff; hiKey = (v >> 8) & 0xff;
            }
            if (HasKey(eff, kGenVelRange)) {
                const int v = GenRaw(eff, kGenVelRange, 0);
                loVel = v & 0xff; hiVel = (v >> 8) & 0xff;
            }
            if (HasKey(presetLayer, kGenKeyRange)) {
                const int v = GenRaw(presetLayer, kGenKeyRange, 0);
                loKey = std::max(loKey, v & 0xff);
                hiKey = std::min(hiKey, (v >> 8) & 0xff);
            }
            if (HasKey(presetLayer, kGenVelRange)) {
                const int v = GenRaw(presetLayer, kGenVelRange, 0);
                loVel = std::max(loVel, v & 0xff);
                hiVel = std::min(hiVel, (v >> 8) & 0xff);
            }
            if (loKey > hiKey || loVel > hiVel) continue;   // empty after intersect

            // Pitch. Tune generators ADD across the two levels.
            const int rootKey = GenOr(eff, kGenOverridingRootKey, smp.originalPitch);
            const int coarse  = GenOr(eff, kGenCoarseTune, 0)
                              + GenOr(presetLayer, kGenCoarseTune, 0);
            const int fine    = GenOr(eff, kGenFineTune, 0)
                              + GenOr(presetLayer, kGenFineTune, 0)
                              + smp.pitchCorrection;

            // Level: attenuation is centibels, and also additive.
            const int attenCb = GenOr(eff, kGenInitialAttenuation, 0)
                              + GenOr(presetLayer, kGenInitialAttenuation, 0);

            // Pan: an explicit generator wins (-500..500 -> -100..100). Absent
            // one, derive from the sample's stereo type so linked left/right
            // samples land hard L/R as the SF2 intends.
            double pan = 0.0;
            if (HasKey(eff, kGenPan) || HasKey(presetLayer, kGenPan)) {
                const int panRaw = GenOr(eff, kGenPan, 0)
                                 + GenOr(presetLayer, kGenPan, 0);
                pan = std::clamp((double)panRaw / 5.0, -100.0, 100.0);
            } else {
                const int type = smp.sampleType & 0x7fff;   // strip the ROM flag
                if      (type == 4) pan = -100.0;           // left
                else if (type == 2) pan =  100.0;           // right
            }

            // Extract the PCM once; later regions on the same sample reuse it.
            auto exIt = extracted.find(sampleIdx);
            if (exIt == extracted.end()) {
                // Preflight against the budget using the DECLARED frame count,
                // so an oversized sample is refused before it is decoded rather
                // than after — the post-check below would otherwise let peak
                // memory reach budget + one whole sample.
                const int64_t declFrames =
                    (int64_t)smp.end - (int64_t)smp.start;
                if (declFrames > 0
                    && declFrames * (int64_t)sizeof(float)
                           > kMaxInstrumentBytes - totalBytes) {
                    overBudget = true;
                    break;
                }
                SampleData sd;
                if (!ExtractSample(in, f.smplOffset, f.smplSize, smp, &sd))
                    continue;
                // ExtractSample bounds ONE sample against the smpl chunk, but a
                // crafted bank can declare a thousand shdr records all spanning
                // that whole chunk, each extracted into its own float vector —
                // measured at 4 GB from a 2 MB file. Bound the running total.
                const int64_t bytes =
                    (int64_t)sd.data.size() * (int64_t)sizeof(float);
                if (totalBytes + bytes > kMaxInstrumentBytes) {
                    // Stop the WHOLE conversion, not just this zone loop: the
                    // outer loop would otherwise keep re-extracting and
                    // re-rejecting samples, and the result would be reported as
                    // a plain success despite the missing regions.
                    overBudget = true;
                    break;
                }
                totalBytes += bytes;
                const int ours = (int)out->samples.size();
                out->samples.push_back(std::move(sd));
                exIt = extracted.emplace(sampleIdx, ours).first;
            }

            // startAddrsOffset shifts where playback begins inside the
            // sample, and is additive across the two generator levels like the
            // tune/attenuation terms. It is expressed in sample frames relative
            // to the sample's own start, which is exactly what Region::offset
            // means for the extracted buffer, so no rebasing is needed and the
            // loop points (already relative to smp.start) stay put.
            //
            // A NEGATIVE total would mean starting before the sample's declared
            // start. Honouring it would require extracting a wider window, but
            // samples are extracted once and shared by SF2 sample index, so one
            // region's negative offset would change what every other region
            // sees. It is clamped to 0 instead (FinalizeInstrument clamps too);
            // negative start offsets do not occur in practice.
            const int startOff = GenOr(eff, kGenStartAddrsOffset, 0)
                               + GenOr(presetLayer, kGenStartAddrsOffset, 0);

            Region r;
            r.sampleIndex    = exIt->second;
            if (startOff > 0) r.offset = startOff;
            r.loKey          = loKey;
            r.hiKey          = hiKey;
            r.loVel          = loVel;
            r.hiVel          = hiVel;
            r.pitchKeycenter = rootKey;
            r.tuneCents      = (float)(coarse * 100 + fine);
            r.volumeDb       = (float)(-(double)attenCb / 10.0);
            r.pan            = (float)pan;

            // scaleTuning is cents per key; SF2's default is 100, same as SFZ's
            // pitch_keytrack. A drum bank sets 0 so hits play at pitch.
            r.pitchKeytrack  = (float)GenOr(eff, kGenScaleTuning, 100);

            // Volume envelope. Timecents -> seconds, centibels -> percent.
            if (HasKey(eff, kGenDelayVolEnv))
                r.ampegDelay = (float)TimecentsToSeconds(GenOr(eff, kGenDelayVolEnv, 0));
            if (HasKey(eff, kGenAttackVolEnv))
                r.ampegAttack = (float)TimecentsToSeconds(GenOr(eff, kGenAttackVolEnv, 0));
            if (HasKey(eff, kGenHoldVolEnv))
                r.ampegHold = (float)TimecentsToSeconds(GenOr(eff, kGenHoldVolEnv, 0));
            if (HasKey(eff, kGenDecayVolEnv))
                r.ampegDecay = (float)TimecentsToSeconds(GenOr(eff, kGenDecayVolEnv, 0));
            if (HasKey(eff, kGenSustainVolEnv)) {
                // SF2 sustain is centibels of attenuation BELOW full level.
                const double cb = (double)GenOr(eff, kGenSustainVolEnv, 0);
                r.ampegSustain = (float)std::clamp(100.0 * std::pow(10.0, -cb / 200.0),
                                                   0.0, 100.0);
            }
            if (HasKey(eff, kGenReleaseVolEnv))
                r.ampegRelease = (float)TimecentsToSeconds(GenOr(eff, kGenReleaseVolEnv, 0));

            // Loop. sampleModes 1 = loop forever, 3 = loop while held then play
            // out the tail. Loop points are absolute into the smpl chunk, so
            // re-base them onto the extracted sample.
            const int modes = GenOr(eff, kGenSampleModes, 0);
            if ((modes == 1 || modes == 3)
                && smp.endLoop > smp.startLoop
                && smp.startLoop >= smp.start && smp.endLoop <= smp.end) {
                r.loopMode  = (modes == 3) ? LoopMode::LoopSustain
                                           : LoopMode::LoopContinuous;
                r.loopStart = (int64_t)smp.startLoop - (int64_t)smp.start;
                r.loopEnd   = (int64_t)smp.endLoop   - (int64_t)smp.start - 1;
            }

            // exclusiveClass is the SF2 spelling of a self-choking group: a new
            // note in the class silences the sounding one. This is what stops
            // an open hi-hat when the closed one is struck.
            const int excl = GenOr(eff, kGenExclusiveClass, 0);
            if (excl != 0) { r.group = excl; r.offBy = excl; }

            out->regions.push_back(r);
        }
        if (overBudget) break;
    }

    FinalizeInstrument(out);

    if (overBudget && warning) {
        char msg[192];
        std::snprintf(msg, sizeof msg,
                      "SF2 preset exceeds the %lld MB sample budget; loaded "
                      "%zu of its samples",
                      (long long)(kMaxInstrumentBytes / (1024 * 1024)),
                      out->samples.size());
        *warning = msg;
    }

    if (out->regions.empty()) {
        if (error)
            *error = "SF2 preset \"" + preset.name + "\" produced no playable regions";
        return false;
    }
    return true;
}

} // namespace daw
