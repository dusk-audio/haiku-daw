#include "ProjectIO.h"

#include "Base64.h"
#include "ClipStretch.h"   // ClampStretchRatio (the clipstretch line's range)
#include "Commands.h"      // kTrackTrimMax (the clamp the loader shares with the command)

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace daw {

namespace {

// Decode the first quoted token on a line. When `escaped` (the new format,
// flagged in the header), content runs from the first '"' to the first
// UNescaped '"', decoding \" \\ \n \r. When !escaped (legacy files written by
// the pre-escape writer), the backslash is a literal byte and the token ends at
// the first '"' — so a legacy path like C:\dir isn't mangled by escape decoding.
std::string Unquote(const std::string& line, bool escaped) {
    size_t a = line.find('"');
    if (a == std::string::npos) return std::string();
    std::string o;
    for (size_t i = a + 1; i < line.size(); i++) {
        char c = line[i];
        if (escaped && c == '\\' && i + 1 < line.size()) {
            char e = line[++i];
            if      (e == 'n') o += '\n';
            else if (e == 'r') o += '\r';
            else               o += e;    // \" \\ and anything else -> literal
        } else if (c == '"') {
            break;                        // closing quote
        } else {
            o += c;
        }
    }
    return o;
}

// Emit s as a quoted, backslash-escaped token so an embedded quote or newline
// can't shift/corrupt the line on read.
std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if      (c == '\\' || c == '"') { o += '\\'; o += c; }
        else if (c == '\n')             o += "\\n";
        else if (c == '\r')             o += "\\r";
        else                            o += c;
    }
    o += '"';
    return o;
}

// Directory portion of a path ("" if none), without the trailing slash.
std::string DirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Store media paths relative to the project file's directory when they live
// under it, so a project + its media are portable together.
std::string Relativize(const std::string& mediaPath, const std::string& baseDir) {
    if (baseDir.empty() || mediaPath.empty())
        return mediaPath;
    const std::string prefix = baseDir + "/";
    if (mediaPath.compare(0, prefix.size(), prefix) == 0)
        return mediaPath.substr(prefix.size());
    return mediaPath;
}

// Resolve a stored path: relative paths are taken relative to the project
// file's directory; absolute paths (leading '/') are left as-is.
std::string Resolve(const std::string& stored, const std::string& baseDir) {
    if (stored.empty() || stored[0] == '/' || baseDir.empty())
        return stored;
    return baseDir + "/" + stored;
}

// Upper bound on any count-prefixed list read from a project line. Guards
// against a corrupt file with a huge count (e.g. "auto gain 2000000000")
// driving an unbounded allocation. Real chains/lanes are far smaller.
constexpr int kMaxListCount = 100000;

// Apply one parsed `fxin`/`masterfxin` record (insert bypass + wet/dry mix) to
// `chain`. Two different malformed-input policies, each matched to the field:
//   - an INDEX that can't address a slot is skipped, like a `send` to a missing
//     track — there is nothing to attach the state to, and failing the whole
//     load over it would lose the rest of a recoverable project;
//   - a MIX outside [0,1] is clamped rather than rejected, because it is
//     meaningless but harmless (unlike an `mev` field, where an out-of-range
//     value means the record itself is garbage, so that one fails the load).
void ApplyFxInsert(std::vector<EffectDesc>& chain, int index, int bypassed,
                   float mix) {
    if (index < 0 || index >= (int)chain.size())
        return;
    chain[(size_t)index].bypassed = (bypassed != 0);
    chain[(size_t)index].mix      = ClampFxMix(mix);
}

// Apply one parsed `fxstate` / `masterfxstate` record (a plugin's own state,
// base64) to `chain`. Same malformed-input policy as ApplyFxInsert: an index
// that addresses no slot is skipped, and so is a token that is not valid base64
// — a plugin's blob is opaque to us, so a damaged one can only be dropped, and
// dropping it leaves the insert at its factory defaults instead of failing a
// whole project over one plugin's unreadable patch.
void ApplyFxState(std::vector<EffectDesc>& chain, int index,
                  const std::string& b64) {
    if (index < 0 || index >= (int)chain.size())
        return;
    std::string decoded;
    if (!Base64Decode(b64, &decoded) || decoded.empty())
        return;
    chain[(size_t)index].state = std::move(decoded);
}

// The base64 token that follows the index on a `fxstate` line, or "" when the
// line has none. Taken as ONE whitespace-free token so nothing a plugin writes
// (a quote, a brace, a newline) can shift the record.
std::string StateToken(const std::string& line) {
    std::istringstream iss(line);
    std::string kw;
    int index = -1;
    iss >> kw >> index;
    std::string b64;
    iss >> b64;
    return b64;
}

// Apply one parsed `fxsc`/`masterfxsc` record (insert -> external sidechain
// source track) to `chain`. Same policy as ApplyFxInsert's INDEX for the same
// reason: a record that cannot address a slot has nothing to attach to, and
// failing the whole load over it would lose the rest of a recoverable project.
// The source is stored as-is rather than resolved here — the track it names may
// be parsed later in the file — and a source that names no track at all (or the
// master sentinel) simply means "no key" when a host resolves it.
void ApplyFxSidechain(std::vector<EffectDesc>& chain, int index, TrackId src) {
    if (index < 0 || index >= (int)chain.size())
        return;
    if (src == kInvalidTrackId)
        return;   // 0 is the descriptor's own "none"; nothing to store
    chain[(size_t)index].sidechainSource = src;
}

} // namespace

bool ProjectIO::Save(const Project& p, const std::string& path) {
    // Atomic-ish save: write a sibling temp file, flush + close, then rename()
    // over the target (rename is atomic on POSIX). A failed/interrupted write
    // (disk full, mid-write crash) leaves the previous good save untouched
    // instead of a truncated file. (No fsync: a power loss between rename and
    // the tmp data reaching disk could still yield a short file — acceptable for
    // v1; add fsync-before-rename if that guarantee is ever required.)
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f) return false;
    // 9 significant digits round-trips a 32-bit float exactly and is plenty for
    // the doubles here (tempo/rate); the default 6 silently drifts gains/pans.
    f << std::setprecision(9);
    const std::string baseDir = DirOf(path);

    // Header: "DAW <major> <fmtFlags>". major=1 (this parser); fmtFlags bit 0 = 1
    // means strings on this file use backslash escaping (Quote/Unquote). Legacy
    // files wrote "DAW 1" with no flag -> read back as literal (unescaped).
    f << "DAW 1 1\n";
    f << "sampleRate " << p.sampleRate << "\n";
    f << "tempo " << p.tempoBPM << " "
      << (p.tempoMap.Tempos().front().ramp ? 1 : 0) << "\n";
    f << "master " << p.masterGain << "\n";
    f << "timesig " << p.timeSig.numerator << " " << p.timeSig.denominator << "\n";
    f << "transport "
      << (long long)p.transport.playhead << " "
      << (p.transport.loopEnabled ? 1 : 0) << " "
      << (long long)p.transport.loopStart << " "
      << (long long)p.transport.loopEnd << "\n";
    f << "punch "
      << (p.transport.punchEnabled ? 1 : 0) << " "
      << (long long)p.transport.punchIn << " "
      << (long long)p.transport.punchOut << "\n";

    for (const EffectDesc& e : p.masterFx) {
        f << "masterfx " << (int)e.type << " " << e.params.size();
        for (float v : e.params) f << " " << v;
        if (EffectHasPluginName(e.type)) f << " " << Quote(e.pluginName);
        f << "\n";
    }
    // Per-insert bypass / wet-dry mix for the master chain, one optional line
    // per NON-DEFAULT insert, written after every `masterfx` line so the index
    // it names already addresses a loaded slot. Skipping the defaults keeps a
    // project that never touches an insert byte-identical to a pre-feature save.
    for (size_t i = 0; i < p.masterFx.size(); i++) {
        const EffectDesc& e = p.masterFx[i];
        if (!e.bypassed && e.mix == 1.0f) continue;
        f << "masterfxin " << i << " " << (e.bypassed ? 1 : 0) << " "
          << e.mix << "\n";
    }
    // The master chain's plugin states, one optional line per insert that has
    // one — the same append-only rule as `masterfxin` above, and written after
    // it so the index already addresses a loaded slot.
    for (size_t i = 0; i < p.masterFx.size(); i++)
        if (!p.masterFx[i].state.empty())
            f << "masterfxstate " << i << " "
              << Base64Encode(p.masterFx[i].state) << "\n";
    // External-sidechain source per master insert (the key is a TRACK either
    // way: the master chain can be ducked by a track, nothing keys the master).
    // One optional line per insert that HAS one, so a project that never uses a
    // sidechain saves byte-identically to a pre-sidechain build's file.
    for (size_t i = 0; i < p.masterFx.size(); i++)
        if (p.masterFx[i].sidechainSource != kInvalidTrackId)
            f << "masterfxsc " << i << " "
              << p.masterFx[i].sidechainSource << "\n";

    // Tempo/meter map. Frame 0 is seeded from `tempo`/`timesig` above, so only
    // changes at frame > 0 are written.
    for (const TempoChange& tc : p.tempoMap.Tempos())
        if (tc.frame > 0)
            f << "tempochange " << (long long)tc.frame << " " << tc.bpm << " "
              << (tc.ramp ? 1 : 0) << "\n";
    for (const MeterChange& mc : p.tempoMap.Meters())
        if (mc.frame > 0)
            f << "meterchange " << (long long)mc.frame << " "
              << mc.num << " " << mc.denom << "\n";

    for (const Marker& mk : p.markers)
        f << "marker " << (long long)mk.frame << " " << Quote(mk.name) << "\n";

    // The project's notes (M3.6). An optional line, written only when there is
    // something to write -- the append-only rule every post-1.0 field follows,
    // so a project that never sets notes saves the bytes it always did.
    if (!p.notes.empty())
        f << "notes " << Quote(p.notes) << "\n";

    for (const Track& t : p.Tracks()) {
        const char* ty = t.type == TrackType::Midi ? "midi"
                       : t.type == TrackType::Bus  ? "bus" : "audio";
        f << "track " << t.id << " " << ty << " "
          << t.gain << " " << t.pan << " "
          << (t.muted ? 1 : 0) << " " << (t.soloed ? 1 : 0) << " "
          << (t.armed ? 1 : 0) << " " << t.output
          << " " << Quote(t.name) << " "
          << t.colorIndex << " " << t.height << " "
          << (t.soloSafe ? 1 : 0) << " " << t.muteGroup << " "
          << (t.inputMonitor ? 1 : 0) << "\n";
        // Track depth (M2.6): a folded lane. Its OWN optional line, like every
        // other post-M2.5 field -- the track line's tail is positional, so a
        // newer build appending there would still be read by an older one only
        // by luck. A project that folds nothing writes the legacy bytes.
        if (t.collapsed)
            f << "trackfold " << t.id << " 1\n";
        // Mixer strip extras (M3.4): input trim, polarity, solo group. One
        // optional line each, written only when the field is not its default,
        // addressed by track id and applied after the whole file is read (see
        // the parser) — so a project that never touches them saves
        // byte-identically to a pre-M3.4 build's file.
        if (t.trim != 1.0f)
            f << "tracktrim " << t.id << " " << t.trim << "\n";
        if (t.phaseInvert)
            f << "trackphase " << t.id << " 1\n";
        if (t.soloGroup > 0)
            f << "tracksologroup " << t.id << " " << t.soloGroup << "\n";

        for (const Clip& c : t.clips) {
            f << "clip " << c.id << " "
              << (long long)c.startFrame << " " << (long long)c.lengthFrames << " "
              << (long long)c.sourceOffset << " "
              << (long long)c.fadeInFrames << " " << (long long)c.fadeOutFrames
              << " " << Quote(Relativize(c.sourcePath, baseDir)) << " "
              << c.gain << " " << c.takeGroup << " " << (c.takeActive ? 1 : 0)
              << "\n";
            // Clip depth (M2.5) rides on its OWN lines, never as extra tail
            // fields: the clip line's optional tail is parsed from its last
            // quote, so a quoted name appended there would silently drop
            // gain/takeGroup/takeActive for every reader. A project that never
            // touches the new fields writes nothing here and stays
            // byte-identical to the old format.
            if (!c.name.empty() || c.colorIndex != 0
                || c.fadeShape != FadeShape::Linear)
                f << "clipinfo " << c.id << " " << c.colorIndex << " "
                  << FadeShapeToInt(c.fadeShape) << " " << Quote(c.name) << "\n";
            if (c.muted)
                f << "clipmute " << c.id << " 1\n";
            // Time-stretch (M7.2): the clip's ratio and (modeled-only) pitch on
            // their own optional line, written only when either is non-default
            // — so a project that never stretches anything saves byte-identically
            // to a pre-stretch build's file. The ratio reads back with the
            // stream's 9-digit precision (see the top of Save), which is finer
            // than a source frame over any real clip length.
            if (ClipIsStretched(c) || c.pitchSemis != 0)
                f << "clipstretch " << c.id << " " << c.stretchRatio << " "
                  << c.pitchSemis << "\n";
        }

        for (const MidiClip& mc : t.midiClips) {
            f << "midiclip " << mc.id << " " << (long long)mc.startFrame << " "
              << (long long)mc.lengthFrames << " " << mc.colorIndex << " "
              << (long long)mc.fadeInFrames << " "
              << (long long)mc.fadeOutFrames << " "
              << mc.takeGroup << " " << (mc.takeActive ? 1 : 0) << "\n";
            if (mc.muted)   // ids are project-unique, so one mute line fits both
                f << "clipmute " << mc.id << " 1\n";
            for (const MidiNote& n : mc.notes)   // frames are clip-relative
                f << "note " << n.pitch << " " << n.velocity << " "
                  << (long long)n.startFrame << " "
                  << (long long)n.lengthFrames << "\n";
            for (const MidiClipEvent& e : mc.events)   // CC/PB/PC/pressure, clip-rel
                f << "mev " << e.type << " " << (long long)e.startFrame << " "
                  << e.data << " " << e.value << "\n";
        }

        for (const EffectDesc& e : t.fx) {
            f << "fx " << (int)e.type << " " << e.params.size();
            for (float v : e.params) f << " " << v;
            if (EffectHasPluginName(e.type)) f << " " << Quote(e.pluginName);
            f << "\n";
        }
        // Per-insert bypass / wet-dry mix, after this track's `fx` lines (see
        // the master chain above for the format rationale).
        for (size_t i = 0; i < t.fx.size(); i++) {
            const EffectDesc& e = t.fx[i];
            if (!e.bypassed && e.mix == 1.0f) continue;
            f << "fxin " << i << " " << (e.bypassed ? 1 : 0) << " "
              << e.mix << "\n";
        }
        // Plugin state (see the master chain above). One line per insert that
        // has one; an insert without state writes nothing at all, so a project
        // of built-ins stays byte-identical to a pre-state save.
        for (size_t i = 0; i < t.fx.size(); i++)
            if (!t.fx[i].state.empty())
                f << "fxstate " << i << " " << Base64Encode(t.fx[i].state)
                  << "\n";
        // External-sidechain source per insert (see the master chain above),
        // written after this track's `fx`/`fxin` lines so the index it names
        // already addresses a loaded slot.
        for (size_t i = 0; i < t.fx.size(); i++)
            if (t.fx[i].sidechainSource != kInvalidTrackId)
                f << "fxsc " << i << " " << t.fx[i].sidechainSource << "\n";

        for (const Send& s : t.sends)
            f << "send " << s.dest << " " << s.level << " "
              << (s.preFader ? 1 : 0) << "\n";

        auto writeLane = [&](const char* which, const AutomationLane& lane) {
            if (lane.Count() == 0) return;
            f << "auto " << which << " " << lane.Count();
            for (size_t i = 0; i < lane.Count(); i++)
                f << " " << (long long)lane.At(i).frame << " " << lane.At(i).value;
            f << "\n";
        };
        writeLane("gain", t.gainAuto);
        writeLane("pan",  t.panAuto);

        for (const FxAutoLane& fa : t.fxAuto) {
            if (fa.lane.Count() == 0) continue;
            f << "fxauto " << fa.fxIndex << " " << fa.slot << " "
              << fa.lane.Count();
            for (size_t i = 0; i < fa.lane.Count(); i++)
                f << " " << (long long)fa.lane.At(i).frame << " "
                  << fa.lane.At(i).value;
            f << "\n";
        }

        if (t.type == TrackType::Midi) {
            // The `instrument` line keeps its exact original 5 fields, always
            // written, so an older build still reads this project (it just
            // plays the synth voice). A soundfont adds a SEPARATE optional
            // line rather than widening this one.
            const Instrument& in = t.instrument.synth;
            f << "instrument " << in.waveform << " " << in.attack << " "
              << in.decay << " " << in.sustain << " " << in.release << "\n";
            if (t.instrument.UsesSoundfont())
                f << "soundfont " << (int)t.instrument.type << " "
                  << t.instrument.sf2Preset << " "
                  << Quote(t.instrument.path) << "\n";
        }

        if (t.input.kind != InputSource::kNone)
            f << "input " << t.input.kind << " " << t.input.channel
              << " " << Quote(t.input.name) << "\n";

        // M5 multichannel addressing: the `input` line above keeps its exact
        // original 3 fields (so an older build still reads this project and
        // records from its default device), and the device + channel pair and
        // the track's channel width are SEPARATE lines, written only when they
        // differ from the default. A file without them loads as the pre-M5
        // default input (device channels 1-2) and a stereo track.
        if (t.input.kind == InputSource::kAudioDefault
            && (!t.input.device.empty() || t.input.channelL != 0
                || t.input.channelR != 1))
            f << "inputdev " << t.input.channelL << " " << t.input.channelR
              << " " << Quote(t.input.device) << "\n";
        if (t.channels != TrackChannels::Stereo)
            f << "trackchans " << (int)t.channels << "\n";

        f << "endtrack\n";
    }
    f << "enddaw\n";      // completeness trailer (see Load: legacy files lack it)
    f.flush();
    const bool ok = f.good();
    f.close();
    if (!ok || !f.good()) { std::remove(tmp.c_str()); return false; }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool ProjectIO::Load(Project& out, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    // Validate the "DAW" magic with a BOUNDED read before touching the project.
    // A bare getline on a binary file (e.g. a WAV opened by mistake) can slurp
    // a huge chunk into a string (memory thrash / apparent freeze); and we must
    // not disturb the current session for a file that turns out not to be ours.
    std::string header;
    bool sawNewline = false;
    for (char c; header.size() < 64 && f.get(c); ) {
        if (c == '\n') { sawNewline = true; break; }
        header.push_back(c);
    }
    // A real header line is newline-terminated within 64 bytes. No newline means
    // a truncated write or a binary file with no line breaks -> reject (a bare
    // "DAW 1" with no trailing newline can't be distinguished from truncation).
    if (!sawNewline)
        return false;
    // Parse the header STRICTLY: exact "DAW" magic + an integer major version,
    // then an OPTIONAL integer fmtFlags, and nothing else. Rejects "DAWSON"
    // (magic is a separate token), "DAW garbage" / missing / malformed version,
    // a malformed fmtFlags, and any extra trailing tokens.
    std::istringstream hs(header);
    std::string magic;
    int major = 0;
    if (!(hs >> magic >> major) || magic != "DAW")
        return false;                 // not our format / no version -> intact
    if (major < 1 || major > 1)       // version gate: only "DAW 1" is parseable
        return false;
    int fmtFlags = 0;
    std::string flagTok;
    if (hs >> flagTok) {              // a third token is present: it MUST be the
        std::istringstream fs(flagTok);   // integer fmtFlags, fully consumed...
        char leftover;
        if (!(fs >> fmtFlags) || (fs >> leftover))
            return false;            // "DAW 1 xyz" / "DAW 1 1x" -> malformed
        std::string extra;
        if (hs >> extra) return false;   // ...and nothing after it ("DAW 1 1 2")
    }
    const bool escaped = (fmtFlags & 1) != 0;   // strings use \-escaping?

    const std::string baseDir = DirOf(path);
    // Parse into a fresh temp project; commit to `out` only after a clean parse
    // so a corrupt/truncated file never wipes the live session.
    Project  p;
    bool     sawTrailer = false;

    Track    cur;
    bool     haveTrack = false;
    TrackId  maxTrack = 0;
    ClipId   maxClip  = 0;
    int      curMidiIdx    = -1;      // index into cur.midiClips for `note` lines
    bool     curMidiLegacy = false;   // current clip was synthesized from old notes

    // Tempo/meter changes are collected and applied after the whole file is
    // read (frame-0 seed depends on `tempo`/`timesig`, which may appear anywhere).
    std::vector<TempoChange> tempoChanges;
    std::vector<MeterChange> meterChanges;
    bool tempo0Ramp = false;   // ramp flag for the frame-0 tempo
    // Track folds, applied once every track exists (see the `trackfold` case).
    std::vector<std::pair<TrackId, bool>> folded;
    // Mixer strip extras (M3.4), same rule and for the same reason: addressed
    // by track id, applied after the whole file has been read, so where the
    // line sits relative to its track record cannot decide whether it survives.
    std::vector<std::pair<TrackId, float>> trims;
    std::vector<std::pair<TrackId, bool>>  phases;
    std::vector<std::pair<TrackId, int>>   soloGroups;

    auto commit = [&]() {
        if (!haveTrack) return;
        // Clips are parsed in file order; enforce the sorted-by-start invariant
        // the engine/crossfade rely on (a hand-edited file may be out of order).
        std::sort(cur.clips.begin(), cur.clips.end(),
                  [](const Clip& a, const Clip& b) {
                      return a.startFrame < b.startFrame;
                  });
        std::sort(cur.midiClips.begin(), cur.midiClips.end(),
                  [](const MidiClip& a, const MidiClip& b) {
                      return a.startFrame < b.startFrame;
                  });
        p.AddTrack(cur);
        haveTrack = false;
    };

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string kw;
        iss >> kw;
        if (kw.empty()) continue;

        if (kw == "sampleRate") { iss >> p.sampleRate; }
        else if (kw == "tempo") {
            iss >> p.tempoBPM;
            int r = 0; if (iss >> r) tempo0Ramp = (r != 0);
        }
        else if (kw == "master") { iss >> p.masterGain; }
        else if (kw == "masterfx") {
            EffectDesc e;
            int type = 0, count = 0;
            iss >> type >> count;
            if (type < 0 || type > kMaxEffectTypeId) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                float v = 0.0f; if (!(iss >> v)) break; e.params.push_back(v);
            }
            if (EffectHasPluginName(e.type)) e.pluginName = Unquote(line, escaped);
            p.masterFx.push_back(e);
        }
        else if (kw == "masterfxin") {
            // Optional; absent in files from before insert slots existed, in
            // which case the chain keeps EffectDesc's defaults (not bypassed,
            // mix 1.0 = fully wet) and behaves exactly as it used to. A
            // truncated line leaves the unread fields at those same defaults.
            int index = -1, byp = 0; float mix = 1.0f;
            iss >> index >> byp >> mix;
            ApplyFxInsert(p.masterFx, index, byp, mix);
        }
        else if (kw == "masterfxstate") {
            // Optional; absent in files from before plugin state was saved, in
            // which case the insert keeps an empty state and the plugin comes
            // up on its own defaults — the pre-feature behaviour, no worse.
            int index = -1;
            iss >> index;
            ApplyFxState(p.masterFx, index, StateToken(line));
        }
        else if (kw == "masterfxsc") {
            // Optional; absent in files from before sidechains existed, in
            // which case the insert keeps EffectDesc's default (no source) and
            // behaves exactly as it used to.
            int index = -1; long long src = 0;
            iss >> index >> src;
            ApplyFxSidechain(p.masterFx, index, src > 0 ? (TrackId)src : kInvalidTrackId);
        }
        else if (kw == "timesig") { iss >> p.timeSig.numerator >> p.timeSig.denominator; }
        else if (kw == "tempochange") {
            long long fr = 0; double bpm = 120.0; int r = 0;
            iss >> fr >> bpm;
            iss >> r;   // optional ramp flag (absent in older files -> 0)
            if (fr > 0) tempoChanges.push_back({(Frame)fr, bpm, r != 0});
        }
        else if (kw == "meterchange") {
            long long fr = 0; int n = 4, d = 4;
            iss >> fr >> n >> d;
            if (fr > 0) meterChanges.push_back({(Frame)fr, n, d});
        }
        else if (kw == "marker") {
            long long fr = 0; iss >> fr;
            Marker mk; mk.frame = (Frame)fr; mk.name = Unquote(line, escaped);
            p.markers.push_back(mk);
        }
        else if (kw == "notes") {
            // The project's own notes (M3.6). Optional: a file without the line
            // loads as a project with no notes, which is what it is.
            p.notes = Unquote(line, escaped);
        }
        else if (kw == "transport") {
            long long ph = 0, ls = 0, le = 0; int loop = 0;
            iss >> ph >> loop >> ls >> le;
            p.transport.playhead = ph;
            p.transport.loopEnabled = (loop != 0);
            p.transport.loopStart = ls;
            p.transport.loopEnd = le;
        }
        else if (kw == "punch") {
            long long pi = 0, po = 0; int en = 0;
            iss >> en >> pi >> po;
            p.transport.punchEnabled = (en != 0);
            p.transport.punchIn = pi;
            p.transport.punchOut = po;
        }
        else if (kw == "track") {
            commit();
            cur = Track{};
            curMidiIdx = -1; curMidiLegacy = false;
            std::string type;
            int mute = 0, solo = 0, arm = 0;
            // These seven are the REQUIRED track fields (present in every format
            // version). A truncated record like "track 1 audio" must fail the
            // load, not silently commit a track with default gain/pan/flags.
            // (Fields after this — output, color, height... — stay optional.)
            if (!(iss >> cur.id >> type >> cur.gain >> cur.pan >> mute >> solo >> arm))
                return false;   // malformed required record; `out` left untouched
            cur.type   = (type == "midi") ? TrackType::Midi
                       : (type == "bus")  ? TrackType::Bus : TrackType::Audio;
            cur.muted  = (mute != 0);
            cur.soloed = (solo != 0);
            cur.armed  = (arm != 0);
            iss >> cur.output;   // routing target; absent in older files -> 0 (master)
            cur.name   = Unquote(line, escaped);
            // Optional color index + lane height after the closing quote.
            if (size_t q = line.rfind('"'); q != std::string::npos) {
                std::istringstream tail(line.substr(q + 1));
                int ci = 0, h = 0, ss = 0, mg = 0, im = 0;
                if (tail >> ci) cur.colorIndex = ci;
                if (tail >> h && h >= 24) cur.height = h;
                if (tail >> ss) cur.soloSafe = (ss != 0);
                if (tail >> mg) cur.muteGroup = mg;
                if (tail >> im) cur.inputMonitor = (im != 0);
            }
            if (cur.id > maxTrack) maxTrack = cur.id;
            haveTrack = true;
        }
        else if (kw == "trackfold") {
            // Track depth (M2.6): the lane is folded. Optional -- absent in
            // every older file, where the track keeps the expanded default --
            // and addressed BY ID and applied after the whole file is read, so
            // the position of the line relative to its track record cannot
            // decide whether it survives (the writer puts it right after the
            // track; a hand-edit or a merge need not).
            long long id = -1; int fold = 0;
            if (!(iss >> id >> fold)) continue;
            folded.emplace_back((TrackId)id, fold != 0);
        }
        else if (kw == "tracktrim") {
            // Optional (M3.4): absent in every older file, where the track
            // keeps the unity default. A value outside the fader's own range
            // is clamped, never rejected: a damaged line must not cost the
            // whole project.
            long long id = -1; float v = 1.0f;
            if (!(iss >> id >> v)) continue;
            trims.emplace_back((TrackId)id, v);
        }
        else if (kw == "trackphase") {
            long long id = -1; int inv = 0;
            if (!(iss >> id >> inv)) continue;
            phases.emplace_back((TrackId)id, inv != 0);
        }
        else if (kw == "tracksologroup") {
            long long id = -1; int g = 0;
            if (!(iss >> id >> g)) continue;
            soloGroups.emplace_back((TrackId)id, g > 0 ? (int)g : 0);
        }
        else if (kw == "clip" && haveTrack) {
            Clip c;
            long long start, len, off, fi, fo;
            iss >> c.id >> start >> len >> off >> fi >> fo;
            c.startFrame = start; c.lengthFrames = len; c.sourceOffset = off;
            c.fadeInFrames = fi; c.fadeOutFrames = fo;
            c.sourcePath = Resolve(Unquote(line, escaped), baseDir);
            // Optional per-clip gain after the closing quote (absent in older
            // files -> 1.0).
            if (size_t q = line.rfind('"'); q != std::string::npos) {
                std::istringstream tail(line.substr(q + 1));
                float g = 1.0f;
                if (tail >> g) c.gain = g;
                int tg = 0, ta = 1;
                if (tail >> tg) c.takeGroup = tg;
                if (tail >> ta) c.takeActive = (ta != 0);
            }
            if (c.id > maxClip) maxClip = c.id;
            cur.clips.push_back(c);
        }
        else if (kw == "clipinfo" && haveTrack) {
            // Clip depth (M2.5): colour, fade shape and display name for an
            // audio clip. Optional; absent in older files, in which case the
            // clip keeps its defaults (inherit the track's colour, a linear
            // fade, the file's base name). A truncated record is skipped
            // rather than applied half-way — these fields are cosmetic, and a
            // half-applied line would clobber what the clip line set.
            int id = -1, color = 0, shape = 0;
            if (!(iss >> id >> color >> shape)) continue;
            for (Clip& c : cur.clips)
                if (c.id == (ClipId)id) {
                    if (color < 0) color = 0;
                    c.colorIndex = color;
                    c.fadeShape  = FadeShapeFromInt(shape);
                    c.name       = Unquote(line, escaped);
                    break;
                }
        }
        else if (kw == "clipstretch" && haveTrack) {
            // Time-stretch (M7.2): a clip's ratio + pitch, addressed by id like
            // the other clip-depth lines. Optional — absent in every older
            // file, where the clip keeps the default (unstretched) — and a
            // truncated record is skipped rather than applied half-way. The
            // ratio is clamped into the model's accepted range: a hand-edited
            // value outside it would otherwise reach the DSP as a stretch it
            // was never designed for.
            int id = -1; double ratio = 1.0; int semis = 0;
            if (!(iss >> id >> ratio)) continue;
            iss >> semis;   // optional (absent in a pitch-less file -> 0)
            for (Clip& c : cur.clips)
                if (c.id == (ClipId)id) {
                    c.stretchRatio = ClampStretchRatio(ratio);
                    c.pitchSemis   = semis;
                    break;
                }
        }
        else if (kw == "clipmute" && haveTrack) {
            // Per-clip / per-region mute (M2.5). Optional, and one keyword for
            // both: clip ids are unique across the two lists.
            int id = -1, muted = 0;
            if (!(iss >> id >> muted)) continue;
            bool done = false;
            for (Clip& c : cur.clips)
                if (c.id == (ClipId)id) { c.muted = (muted != 0); done = true; break; }
            if (!done)
                for (MidiClip& c : cur.midiClips)
                    if (c.id == (ClipId)id) { c.muted = (muted != 0); break; }
        }
        else if (kw == "midiclip" && haveTrack) {
            MidiClip mc;
            long long id = 0, st = 0, ln = 0; int col = 0;
            long long fi = 0, fo = 0;
            iss >> id >> st >> ln >> col;
            iss >> fi >> fo;   // optional fades (absent in older files -> 0)
            int tg = 0, ta = 1;
            iss >> tg >> ta;   // optional take group/active (absent -> 0/on)
            mc.id = (ClipId)id; mc.startFrame = (Frame)st;
            mc.lengthFrames = (Frame)ln; mc.colorIndex = col;
            mc.fadeInFrames = (Frame)fi; mc.fadeOutFrames = (Frame)fo;
            mc.takeGroup = tg; mc.takeActive = (ta != 0);
            if (mc.id > maxClip) maxClip = mc.id;
            cur.midiClips.push_back(mc);
            curMidiIdx = (int)cur.midiClips.size() - 1;
            curMidiLegacy = false;
        }
        else if (kw == "note" && haveTrack) {
            MidiNote n;
            long long start, len;
            iss >> n.pitch >> n.velocity >> start >> len;
            n.startFrame = start; n.lengthFrames = len;
            // Legacy files stored track-absolute notes with no enclosing clip;
            // migrate them into one implicit region at frame 0 whose window
            // grows to cover them (notes stay absolute == relative-to-0).
            if (curMidiIdx < 0) {
                MidiClip mc; mc.id = kInvalidClipId;   // id assigned post-load
                mc.startFrame = 0; mc.lengthFrames = 1;
                cur.midiClips.push_back(mc);
                curMidiIdx = (int)cur.midiClips.size() - 1;
                curMidiLegacy = true;
            }
            cur.midiClips[(size_t)curMidiIdx].notes.push_back(n);
            if (curMidiLegacy) {
                const Frame end = n.startFrame + n.lengthFrames;
                MidiClip& mc = cur.midiClips[(size_t)curMidiIdx];
                if (end > mc.lengthFrames) mc.lengthFrames = end;
            }
        }
        else if (kw == "mev" && haveTrack && curMidiIdx >= 0) {
            MidiClipEvent e;
            long long st = 0;
            // A failed extraction leaves these ZEROED, which passes the range
            // check below and would store a bogus CC at frame 0. Fail the load
            // instead, like the `track` record does — the temp project is
            // discarded, so the live session is untouched.
            if (!(iss >> e.type >> st >> e.data >> e.value))
                return false;
            // An unknown type is FORWARD compatibility, not corruption — a newer
            // build may define more — so skip it, the same way an unknown line
            // keyword is skipped. Everything below only has to hold for the
            // types this build actually interprets.
            if (e.type < MidiClipEvent::CC || e.type > MidiClipEvent::ChannelPressure)
                continue;
            // Range-check before storing: NOTHING downstream re-checks these.
            // MidiChannelGain (MidiControl.h) divides the raw CC7/CC11 values by
            // 127 with no clamp, so a stored value of 65535 is a ~516x gain on
            // that track's notes, and PitchBendAt returns e.value verbatim. A
            // record that cannot be interpreted is corruption, so fail the load
            // like the `track` record does — the temp project is discarded and
            // the live session is untouched.
            const int maxValue = (e.type == MidiClipEvent::PitchBend) ? 16383 : 127;
            if (st < 0 || e.data < 0 || e.data > 127
                || e.value < 0 || e.value > maxValue)
                return false;
            e.startFrame = (Frame)st;
            cur.midiClips[(size_t)curMidiIdx].events.push_back(e);
        }
        else if (kw == "fx" && haveTrack) {
            EffectDesc e;
            int type = 0, count = 0;
            iss >> type >> count;
            if (type < 0 || type > kMaxEffectTypeId) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                float v = 0.0f;
                if (!(iss >> v)) break;
                e.params.push_back(v);
            }
            if (EffectHasPluginName(e.type)) e.pluginName = Unquote(line, escaped);
            cur.fx.push_back(e);
        }
        else if (kw == "fxin" && haveTrack) {
            int index = -1, byp = 0; float mix = 1.0f;
            iss >> index >> byp >> mix;
            ApplyFxInsert(cur.fx, index, byp, mix);
        }
        else if (kw == "fxstate" && haveTrack) {
            int index = -1;
            iss >> index;
            ApplyFxState(cur.fx, index, StateToken(line));
        }
        else if (kw == "fxsc" && haveTrack) {
            int index = -1; long long src = 0;
            iss >> index >> src;
            ApplyFxSidechain(cur.fx, index, src > 0 ? (TrackId)src : kInvalidTrackId);
        }
        else if (kw == "fxauto" && haveTrack) {
            FxAutoLane fa;
            int count = 0;
            iss >> fa.fxIndex >> fa.slot >> count;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                long long fr = 0; float v = 0.0f;
                if (!(iss >> fr >> v)) break;
                fa.lane.AddPoint((Frame)fr, v);
            }
            if (fa.lane.Count() > 0) cur.fxAuto.push_back(fa);
        }
        else if (kw == "instrument" && haveTrack) {
            Instrument in;
            iss >> in.waveform >> in.attack >> in.decay >> in.sustain
                >> in.release;
            if (in.waveform < 0 || in.waveform > 3) in.waveform = 0;
            cur.instrument.synth = in;
        }
        else if (kw == "soundfont" && haveTrack) {
            int type = 0, preset = 0;
            iss >> type >> preset;
            // Clamp into range like the waveform above: a file from a newer
            // build (or a corrupt one) must not select a voice this build
            // cannot make. Falling back to Synth keeps the track audible.
            if (type < 0 || type > kMaxInstrumentTypeId)
                type = (int)InstrumentType::Synth;
            if (preset < 0) preset = 0;
            cur.instrument.type      = (InstrumentType)type;
            cur.instrument.sf2Preset = preset;
            cur.instrument.path      = Unquote(line, escaped);
            // A soundfont voice with no path can never load; keep it on the
            // synth rather than leaving a broken descriptor behind.
            if (cur.instrument.path.empty())
                cur.instrument.type = InstrumentType::Synth;
        }
        else if (kw == "input" && haveTrack) {
            InputSource in = cur.input;   // keep an `inputdev` already read
            iss >> in.kind >> in.channel;
            if (in.kind < 0 || in.kind > InputSource::kMidi) in.kind = InputSource::kNone;
            in.name = Unquote(line, escaped);   // "" if the line has no quotes
            cur.input = in;
        }
        else if (kw == "inputdev" && haveTrack) {
            // M5: the audio device + channel pair, a separate optional line.
            // A negative channel is clamped away rather than kept -- it would
            // address outside a capture block.
            int l = 0, r = 1;
            iss >> l >> r;
            cur.input.channelL = l < 0 ? 0 : l;
            cur.input.channelR = r < 0 ? cur.input.channelL : r;
            cur.input.device   = Unquote(line, escaped);
        }
        else if (kw == "trackchans" && haveTrack) {
            // M5: 0 = mono, 1 = stereo. Anything else reads as stereo, which
            // is what a file without the line means.
            int v = 1;
            iss >> v;
            cur.channels = (v == 0) ? TrackChannels::Mono : TrackChannels::Stereo;
        }
        else if (kw == "send" && haveTrack) {
            Send s;
            int pre = 0;
            iss >> s.dest >> s.level >> pre;
            s.preFader = (pre != 0);
            if (s.dest != kInvalidTrackId) cur.sends.push_back(s);
        }
        else if (kw == "auto" && haveTrack) {
            std::string which;
            int count = 0;
            iss >> which >> count;
            AutomationLane& lane = (which == "pan") ? cur.panAuto : cur.gainAuto;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                long long fr = 0; float v = 0.0f;
                if (!(iss >> fr >> v)) break;
                lane.AddPoint((Frame)fr, v);
            }
        }
        else if (kw == "enddaw") { sawTrailer = true; break; }
        // "endtrack" and unknown keywords: ignored (commit happens on next
        // track / EOF).
    }
    commit();
    // A missing `enddaw` can't be distinguished from a legacy pre-trailer file
    // (both carry "DAW 1"), so a truncated file is still accepted here — the
    // real corruption defenses are the temp-parse-then-swap (a bad file never
    // wipes the live session) and the version gate. Bumping the magic to "DAW 2"
    // would make trailer-absence a reliable truncation signal; deferred so
    // existing v1 files keep loading.
    (void)sawTrailer;

    // Build the tempo/meter map: seed frame 0 from tempo/timesig, sync sample
    // rate, then apply the collected changes.
    p.tempoMap.sampleRate = p.sampleRate;
    p.tempoMap.Reset(p.tempoBPM, p.timeSig.numerator, p.timeSig.denominator);
    p.tempoMap.SetTempoAt(0, p.tempoBPM, tempo0Ramp);
    for (const TempoChange& tc : tempoChanges)
        p.tempoMap.SetTempoAt(tc.frame, tc.bpm, tc.ramp);
    for (const MeterChange& mc : meterChanges) p.tempoMap.SetMeterAt(mc.frame, mc.num, mc.denom);

    p.ReserveIds(maxTrack, maxClip);
    // Give migrated (legacy) MIDI regions real ids now that maxClip is known.
    for (Track& t : p.Tracks())
        for (MidiClip& mc : t.midiClips)
            if (mc.id == kInvalidClipId) mc.id = p.NextClipId();
    // Track folds, now that every track record has been committed.
    for (const auto& f : folded)
        if (Track* t = p.FindTrack(f.first)) t->collapsed = f.second;
    // The mixer strip extras (M3.4), same place and same reason. The trim is
    // clamped here as well as in the command: a hand-edited file is input too.
    for (const auto& e : trims) {
        if (Track* t = p.FindTrack(e.first)) {
            float v = e.second;
            if (!(v >= 0.0f)) v = 0.0f;
            if (v > kTrackTrimMax) v = kTrackTrimMax;
            t->trim = v;
        }
    }
    for (const auto& e : phases)
        if (Track* t = p.FindTrack(e.first)) t->phaseInvert = e.second;
    for (const auto& e : soloGroups)
        if (Track* t = p.FindTrack(e.first)) t->soloGroup = e.second;

    out = std::move(p);   // clean parse -> commit; live session was untouched
    return true;
}

} // namespace daw
