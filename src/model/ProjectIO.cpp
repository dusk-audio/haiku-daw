#include "ProjectIO.h"

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
        if (e.type == EffectType::Plugin) f << " " << Quote(e.pluginName);
        f << "\n";
    }

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

        for (const Clip& c : t.clips)
            f << "clip " << c.id << " "
              << (long long)c.startFrame << " " << (long long)c.lengthFrames << " "
              << (long long)c.sourceOffset << " "
              << (long long)c.fadeInFrames << " " << (long long)c.fadeOutFrames
              << " " << Quote(Relativize(c.sourcePath, baseDir)) << " "
              << c.gain << " " << c.takeGroup << " " << (c.takeActive ? 1 : 0)
              << "\n";

        for (const MidiClip& mc : t.midiClips) {
            f << "midiclip " << mc.id << " " << (long long)mc.startFrame << " "
              << (long long)mc.lengthFrames << " " << mc.colorIndex << " "
              << (long long)mc.fadeInFrames << " "
              << (long long)mc.fadeOutFrames << " "
              << mc.takeGroup << " " << (mc.takeActive ? 1 : 0) << "\n";
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
            if (e.type == EffectType::Plugin) f << " " << Quote(e.pluginName);
            f << "\n";
        }

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
            const Instrument& in = t.instrument;
            f << "instrument " << in.waveform << " " << in.attack << " "
              << in.decay << " " << in.sustain << " " << in.release << "\n";
        }

        if (t.input.kind != InputSource::kNone)
            f << "input " << t.input.kind << " " << t.input.channel
              << " " << Quote(t.input.name) << "\n";

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
            if (type < 0 || type > 8) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                float v = 0.0f; if (!(iss >> v)) break; e.params.push_back(v);
            }
            if (e.type == EffectType::Plugin) e.pluginName = Unquote(line, escaped);
            p.masterFx.push_back(e);
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
            iss >> e.type >> st >> e.data >> e.value;
            e.startFrame = (Frame)st;
            // Ignore an unknown/garbage event type rather than store it.
            if (e.type >= MidiClipEvent::CC && e.type <= MidiClipEvent::ChannelPressure)
                cur.midiClips[(size_t)curMidiIdx].events.push_back(e);
        }
        else if (kw == "fx" && haveTrack) {
            EffectDesc e;
            int type = 0, count = 0;
            iss >> type >> count;
            if (type < 0 || type > 8) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count && i < kMaxListCount; i++) {
                float v = 0.0f;
                if (!(iss >> v)) break;
                e.params.push_back(v);
            }
            if (e.type == EffectType::Plugin) e.pluginName = Unquote(line, escaped);
            cur.fx.push_back(e);
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
            cur.instrument = in;
        }
        else if (kw == "input" && haveTrack) {
            InputSource in;
            iss >> in.kind >> in.channel;
            if (in.kind < 0 || in.kind > InputSource::kMidi) in.kind = InputSource::kNone;
            in.name = Unquote(line, escaped);   // "" if the line has no quotes
            cur.input = in;
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

    out = std::move(p);   // clean parse -> commit; live session was untouched
    return true;
}

} // namespace daw
