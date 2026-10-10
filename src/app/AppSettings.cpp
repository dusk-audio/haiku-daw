#include "AppSettings.h"

#include <algorithm>
#include <sstream>

namespace daw {

void AppSettings::RememberRecent(std::vector<std::string>& recent,
                                 const std::string& path) {
    if (path.empty()) return;
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > kMaxRecent) recent.resize(kMaxRecent);
}

std::string AppSettings::Serialize() const {
    std::ostringstream o;
    o << "buffer "   << bufferFrames << "\n"
      << "countin "  << countInBars  << "\n"
      << "metronome " << (metronome ? 1 : 0) << "\n"
      << "monitorin " << (monitorInput ? 1 : 0) << "\n"
      << "win " << winL << " " << winT << " " << winR << " " << winB << "\n"
      << "uiinsp " << (inspectorVisible ? 1 : 0) << " " << inspectorWidth << "\n"
      << "uibottom " << (bottomVisible ? 1 : 0) << " " << bottomHeight << "\n"
      << "expcont "   << exportContainer << "\n"
      << "expbits "   << exportBitDepth << "\n"
      << "expqual "   << exportVorbisQuality << "\n"
      << "expdither " << (exportDither ? 1 : 0) << "\n"
      << "exprate "   << exportSampleRate << "\n"
      << "expnorm "   << (exportNormalize ? 1 : 0) << "\n"
      << "explufs "   << exportTargetLufs << "\n"
      << "expceil "   << exportTruePeakCeil << "\n"
      << "explim "    << (exportLimiter ? 1 : 0) << "\n"
      << "exprange "  << exportRange << "\n"
      << "expstems "  << exportStems << "\n";
    // The recent list is one line per entry (paths may contain spaces -> rest
    // of the line, like lastdir below).
    for (const std::string& r : recentProjects)
        o << "recent " << r << "\n";
    // lastDir last (may contain spaces -> rest of the line).
    o << "lastdir " << lastDir << "\n";
    return o.str();
}

bool AppSettings::Deserialize(const std::string& text) {
    if (text.empty()) return false;
    // A parse is the whole state: the list would otherwise accumulate across
    // repeated parses into one object.
    recentProjects.clear();
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw.empty()) continue;
        any = true;
        // Read into a temp and only assign on success: a failed operator>>
        // sets the target to 0, which would clobber a sane default.
        if      (kw == "buffer")    { int v; if (ls >> v) bufferFrames = v; }
        else if (kw == "countin")   { int v; if (ls >> v) countInBars = v; }
        else if (kw == "metronome") { int v; if (ls >> v) metronome = (v != 0); }
        else if (kw == "monitorin") { int v; if (ls >> v) monitorInput = (v != 0); }
        else if (kw == "win") {
            float l, t, r, b;
            if (ls >> l >> t >> r >> b) { winL = l; winT = t; winR = r; winB = b; }
        }
        else if (kw == "uiinsp") {
            int v; float w;
            if (ls >> v >> w) { inspectorVisible = (v != 0); inspectorWidth = w; }
        }
        else if (kw == "uibottom") {
            int v; float h;
            if (ls >> v >> h) { bottomVisible = (v != 0); bottomHeight = h; }
        }
        else if (kw == "expcont")   { int v; if (ls >> v) exportContainer = v; }
        else if (kw == "expbits")   { int v; if (ls >> v) exportBitDepth = v; }
        else if (kw == "expqual")   { float v; if (ls >> v) exportVorbisQuality = v; }
        else if (kw == "expdither") { int v; if (ls >> v) exportDither = (v != 0); }
        else if (kw == "exprate")   { int v; if (ls >> v) exportSampleRate = v; }
        else if (kw == "expnorm")   { int v; if (ls >> v) exportNormalize = (v != 0); }
        else if (kw == "explufs")   { float v; if (ls >> v) exportTargetLufs = v; }
        else if (kw == "expceil")   { float v; if (ls >> v) exportTruePeakCeil = v; }
        else if (kw == "explim")    { int v; if (ls >> v) exportLimiter = (v != 0); }
        else if (kw == "exprange")  { int v; if (ls >> v) exportRange = v; }
        else if (kw == "expstems")  { int v; if (ls >> v) exportStems = v; }
        else if (kw == "recent") {
            std::string rest;
            std::getline(ls, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            if (!rest.empty() && recentProjects.size() < kMaxRecent)
                recentProjects.push_back(rest);
        }
        else if (kw == "lastdir") {
            std::string rest;
            std::getline(ls, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            lastDir = rest;
        }
        // unknown keys ignored
    }
    return any;
}

} // namespace daw
