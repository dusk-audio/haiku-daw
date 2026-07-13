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

} // namespace

bool ProjectIO::Save(const Project& p, const std::string& path) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;

    f << "DAW 1\n";
    f << "sampleRate " << p.sampleRate << "\n";
    f << "tempo " << p.tempoBPM << "\n";
    f << "timesig " << p.timeSig.numerator << " " << p.timeSig.denominator << "\n";
    f << "transport "
      << (long long)p.transport.playhead << " "
      << (p.transport.loopEnabled ? 1 : 0) << " "
      << (long long)p.transport.loopStart << " "
      << (long long)p.transport.loopEnd << "\n";

    for (const Track& t : p.Tracks()) {
        f << "track " << t.id << " "
          << (t.type == TrackType::Midi ? "midi" : "audio") << " "
          << t.gain << " " << t.pan << " "
          << (t.muted ? 1 : 0) << " " << (t.soloed ? 1 : 0) << " "
          << (t.armed ? 1 : 0) << " \"" << t.name << "\"\n";

        for (const Clip& c : t.clips)
            f << "clip " << c.id << " "
              << (long long)c.startFrame << " " << (long long)c.lengthFrames << " "
              << (long long)c.sourceOffset << " "
              << (long long)c.fadeInFrames << " " << (long long)c.fadeOutFrames
              << " \"" << c.sourcePath << "\"\n";

        for (const MidiNote& n : t.notes)
            f << "note " << n.pitch << " " << n.velocity << " "
              << (long long)n.startFrame << " " << (long long)n.lengthFrames << "\n";

        for (const EffectDesc& e : t.fx)
            f << "fx " << (e.type == EffectType::Delay ? 1 : 0) << " "
              << e.p0 << " " << e.p1 << " " << e.p2 << " " << e.p3 << "\n";

        f << "endtrack\n";
    }
    return f.good();
}

bool ProjectIO::Load(Project& p, const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    p.Clear();

    std::string header;
    std::getline(f, header);
    if (header.rfind("DAW", 0) != 0)   // must start with "DAW"
        return false;

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
            cur.type   = (type == "midi") ? TrackType::Midi : TrackType::Audio;
            cur.muted  = (mute != 0);
            cur.soloed = (solo != 0);
            cur.armed  = (arm != 0);
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
            c.sourcePath = Unquote(line);
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
            int type;
            iss >> type >> e.p0 >> e.p1 >> e.p2 >> e.p3;
            e.type = (type == 1) ? EffectType::Delay : EffectType::Biquad;
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
