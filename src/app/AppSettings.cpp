#include "AppSettings.h"

#include <sstream>

namespace daw {

std::string AppSettings::Serialize() const {
    std::ostringstream o;
    o << "buffer "   << bufferFrames << "\n"
      << "countin "  << countInBars  << "\n"
      << "metronome " << (metronome ? 1 : 0) << "\n"
      << "monitorin " << (monitorInput ? 1 : 0) << "\n"
      << "win " << winL << " " << winT << " " << winR << " " << winB << "\n";
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
        if      (kw == "buffer")    ls >> bufferFrames;
        else if (kw == "countin")   ls >> countInBars;
        else if (kw == "metronome") { int v = 0; ls >> v; metronome = (v != 0); }
        else if (kw == "monitorin") { int v = 0; ls >> v; monitorInput = (v != 0); }
        else if (kw == "win")       ls >> winL >> winT >> winR >> winB;
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
