#include "AppSettings.h"

#include <sstream>

namespace daw {

std::string AppSettings::Serialize() const {
    std::ostringstream o;
    o << "buffer "   << bufferFrames << "\n"
      << "countin "  << countInBars  << "\n"
      << "metronome " << (metronome ? 1 : 0) << "\n"
      << "monitorin " << (monitorInput ? 1 : 0) << "\n"
      << "win " << winL << " " << winT << " " << winR << " " << winB << "\n"
      << "expbits "   << exportBitDepth << "\n"
      << "expdither " << (exportDither ? 1 : 0) << "\n"
      << "exprate "   << exportSampleRate << "\n"
      << "expnorm "   << (exportNormalize ? 1 : 0) << "\n"
      << "explufs "   << exportTargetLufs << "\n"
      << "expceil "   << exportTruePeakCeil << "\n"
      << "explim "    << (exportLimiter ? 1 : 0) << "\n"
      << "exprange "  << exportRange << "\n"
      << "expstems "  << exportStems << "\n";
    // lastDir last (may contain spaces -> rest of the line).
    o << "lastdir " << lastDir << "\n";
    return o.str();
}

bool AppSettings::Deserialize(const std::string& text) {
    if (text.empty()) return false;
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
        else if (kw == "expbits")   { int v; if (ls >> v) exportBitDepth = v; }
        else if (kw == "expdither") { int v; if (ls >> v) exportDither = (v != 0); }
        else if (kw == "exprate")   { int v; if (ls >> v) exportSampleRate = v; }
        else if (kw == "expnorm")   { int v; if (ls >> v) exportNormalize = (v != 0); }
        else if (kw == "explufs")   { float v; if (ls >> v) exportTargetLufs = v; }
        else if (kw == "expceil")   { float v; if (ls >> v) exportTruePeakCeil = v; }
        else if (kw == "explim")    { int v; if (ls >> v) exportLimiter = (v != 0); }
        else if (kw == "exprange")  { int v; if (ls >> v) exportRange = v; }
        else if (kw == "expstems")  { int v; if (ls >> v) exportStems = v; }
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
