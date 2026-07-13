#include "ProjectIO.h"

#include <fstream>
#include <sstream>

namespace daw {

namespace {

// Extract the substring between the first and last double-quote on a line.
// Returns "" if there aren't two quotes.
std::string Unquote(const std::string& line) {
    size_t a = line.find('"');
    size_t b = line.rfind('"');
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return std::string();
    return line.substr(a + 1, b - a - 1);
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

} // namespace

bool ProjectIO::Save(const Project& p, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    const std::string baseDir = DirOf(path);

    f << "DAW 1\n";
    f << "sampleRate " << p.sampleRate << "\n";
    f << "tempo " << p.tempoBPM << "\n";
    f << "master " << p.masterGain << "\n";
    f << "timesig " << p.timeSig.numerator << " " << p.timeSig.denominator << "\n";
    f << "transport "
      << (long long)p.transport.playhead << " "
      << (p.transport.loopEnabled ? 1 : 0) << " "
      << (long long)p.transport.loopStart << " "
      << (long long)p.transport.loopEnd << "\n";

    for (const EffectDesc& e : p.masterFx) {
        f << "masterfx " << (int)e.type << " " << e.params.size();
        for (float v : e.params) f << " " << v;
        f << "\n";
    }

    for (const Track& t : p.Tracks()) {
        const char* ty = t.type == TrackType::Midi ? "midi"
                       : t.type == TrackType::Bus  ? "bus" : "audio";
        f << "track " << t.id << " " << ty << " "
          << t.gain << " " << t.pan << " "
          << (t.muted ? 1 : 0) << " " << (t.soloed ? 1 : 0) << " "
          << (t.armed ? 1 : 0) << " " << t.output
          << " \"" << t.name << "\"\n";

        for (const Clip& c : t.clips)
            f << "clip " << c.id << " "
              << (long long)c.startFrame << " " << (long long)c.lengthFrames << " "
              << (long long)c.sourceOffset << " "
              << (long long)c.fadeInFrames << " " << (long long)c.fadeOutFrames
              << " \"" << Relativize(c.sourcePath, baseDir) << "\"\n";

        for (const MidiNote& n : t.notes)
            f << "note " << n.pitch << " " << n.velocity << " "
              << (long long)n.startFrame << " " << (long long)n.lengthFrames << "\n";

        for (const EffectDesc& e : t.fx) {
            f << "fx " << (int)e.type << " " << e.params.size();
            for (float v : e.params) f << " " << v;
            f << "\n";
        }

        f << "endtrack\n";
    }
    return f.good();
}

bool ProjectIO::Load(Project& p, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    // Validate the "DAW" magic with a BOUNDED read before touching the project.
    // A bare getline on a binary file (e.g. a WAV opened by mistake) can slurp
    // a huge chunk into a string (memory thrash / apparent freeze); and we must
    // not Clear() the current session for a file that turns out not to be ours.
    std::string header;
    for (char c; header.size() < 64 && f.get(c) && c != '\n'; )
        header.push_back(c);
    if (header.rfind("DAW", 0) != 0)   // not our format -> leave project intact
        return false;

    const std::string baseDir = DirOf(path);
    p.Clear();

    Track    cur;
    bool     haveTrack = false;
    TrackId  maxTrack = 0;
    ClipId   maxClip  = 0;

    auto commit = [&]() {
        if (haveTrack) { p.AddTrack(cur); haveTrack = false; }
    };

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string kw;
        iss >> kw;
        if (kw.empty()) continue;

        if (kw == "sampleRate") { iss >> p.sampleRate; }
        else if (kw == "tempo") { iss >> p.tempoBPM; }
        else if (kw == "master") { iss >> p.masterGain; }
        else if (kw == "masterfx") {
            EffectDesc e;
            int type = 0, count = 0;
            iss >> type >> count;
            if (type < 0 || type > 7) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count; i++) { float v = 0.0f; iss >> v; e.params.push_back(v); }
            p.masterFx.push_back(e);
        }
        else if (kw == "timesig") { iss >> p.timeSig.numerator >> p.timeSig.denominator; }
        else if (kw == "transport") {
            long long ph, ls, le; int loop;
            iss >> ph >> loop >> ls >> le;
            p.transport.playhead = ph;
            p.transport.loopEnabled = (loop != 0);
            p.transport.loopStart = ls;
            p.transport.loopEnd = le;
        }
        else if (kw == "track") {
            commit();
            cur = Track{};
            std::string type;
            int mute, solo, arm;
            iss >> cur.id >> type >> cur.gain >> cur.pan >> mute >> solo >> arm;
            cur.type   = (type == "midi") ? TrackType::Midi
                       : (type == "bus")  ? TrackType::Bus : TrackType::Audio;
            cur.muted  = (mute != 0);
            cur.soloed = (solo != 0);
            cur.armed  = (arm != 0);
            iss >> cur.output;   // routing target; absent in older files -> 0 (master)
            cur.name   = Unquote(line);
            if (cur.id > maxTrack) maxTrack = cur.id;
            haveTrack = true;
        }
        else if (kw == "clip" && haveTrack) {
            Clip c;
            long long start, len, off, fi, fo;
            iss >> c.id >> start >> len >> off >> fi >> fo;
            c.startFrame = start; c.lengthFrames = len; c.sourceOffset = off;
            c.fadeInFrames = fi; c.fadeOutFrames = fo;
            c.sourcePath = Resolve(Unquote(line), baseDir);
            if (c.id > maxClip) maxClip = c.id;
            cur.clips.push_back(c);
        }
        else if (kw == "note" && haveTrack) {
            MidiNote n;
            long long start, len;
            iss >> n.pitch >> n.velocity >> start >> len;
            n.startFrame = start; n.lengthFrames = len;
            cur.notes.push_back(n);
        }
        else if (kw == "fx" && haveTrack) {
            EffectDesc e;
            int type = 0, count = 0;
            iss >> type >> count;
            if (type < 0 || type > 7) type = 0;
            e.type = (EffectType)type;
            for (int i = 0; i < count; i++) {
                float v = 0.0f;
                iss >> v;
                e.params.push_back(v);
            }
            cur.fx.push_back(e);
        }
        // "endtrack" and unknown keywords: ignored (commit happens on next
        // track / EOF).
    }
    commit();

    p.ReserveIds(maxTrack, maxClip);
    return true;
}

} // namespace daw
