#include "AppSettings.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace daw {

namespace {

// The member of `set` closest to `v` (ties go to the smaller). Used for a
// preference whose window offers a fixed set: a file that says 500 frames
// means the 512 entry, not "unusable, fall back to the default".
int NearestInSet(int v, const int* set, int count) {
    int best = set[0];
    long bestDist = -1;
    for (int i = 0; i < count; i++) {
        const long d = std::labs((long)v - (long)set[i]);
        if (bestDist < 0 || d < bestDist) { best = set[i]; bestDist = d; }
    }
    return best;
}

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

void AppSettings::Sanitize() {
    // A non-positive value is not a small buffer, it is a missing one: it
    // means the file has garbage where the number goes, so the DEFAULT comes
    // back (0 frames would divide by zero in the audio path).
    bufferFrames = bufferFrames <= 0
                 ? 512
                 : NearestInSet(bufferFrames, kBufferFrameChoices,
                                kBufferFrameChoiceCount);
    defaultSampleRate = defaultSampleRate <= 0
                      ? 48000
                      : NearestInSet(defaultSampleRate, kSampleRateChoices,
                                     kSampleRateChoiceCount);
    // The count-in is bars, and there is no sense in more than a bar or two of
    // them: clamp rather than snap, so a future "3 bars" still loads as 3.
    countInBars = ClampInt(countInBars, 0, 8);
    metronomeSound = ClampInt(metronomeSound, 0, kMetronomeSoundCount - 1);
    if (!std::isfinite(metronomeLevel)) metronomeLevel = 0.30f;
    metronomeLevel = std::min(std::max(metronomeLevel, 0.0f), 1.0f);
    // 0 is the "follow the system font" sentinel and stays 0; a forced scale
    // is clamped to what Themed() geometry stays usable at.
    if (!std::isfinite(uiScale) || uiScale < 0.0f) uiScale = 0.0f;
    if (uiScale > 3.0f) uiScale = 3.0f;
    // Autosave: 0 is off; anything between "off" and a sane interval becomes
    // the shortest sane one (a 1 s recovery write is a footgun, not a choice).
    if (autosaveSeconds < 0) autosaveSeconds = 0;
    if (autosaveSeconds > 0 && autosaveSeconds < 5) autosaveSeconds = 5;
    if (autosaveSeconds > 3600) autosaveSeconds = 3600;
}

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
      << "uidockpage " << dockPage << "\n"
      << "uimarklane " << (markerLaneVisible ? 1 : 0) << "\n"
      << "expbits "   << exportBitDepth << "\n"
      << "expqual "   << exportVorbisQuality << "\n"
      << "expdither " << (exportDither ? 1 : 0) << "\n"
      << "exprate "   << exportSampleRate << "\n"
      << "expnorm "   << (exportNormalize ? 1 : 0) << "\n"
      << "explufs "   << exportTargetLufs << "\n"
      << "expceil "   << exportTruePeakCeil << "\n"
      << "explim "    << (exportLimiter ? 1 : 0) << "\n"
      << "exprange "  << exportRange << "\n"
      << "expstems "  << exportStems << "\n"
      << "theme "     << ThemeModeToString(themeMode) << "\n"
      << "takefmt "   << takeFormat << "\n"
      // The Preferences window's own keys (M3.5). Optional on load like every
      // line here: a file written by an older build simply has none of them
      // and keeps the defaults.
      << "defrate "   << defaultSampleRate << "\n"
      << "metsnd "    << metronomeSound << "\n"
      << "metlvl "    << metronomeLevel << "\n"
      << "uiscale "   << uiScale << "\n"
      << "autosv "    << autosaveSeconds << "\n"
      << "audprev "   << (previewNotes ? 1 : 0) << "\n";
    // The default project folder (M3.5) may contain spaces -> rest of the
    // line, like lastdir below.
    o << "projdir " << projectDir << "\n";
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
        else if (kw == "uidockpage") { int v; if (ls >> v) dockPage = v; }
        else if (kw == "uimarklane") { int v; if (ls >> v) markerLaneVisible = (v != 0); }
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
        else if (kw == "theme") {
            std::string v;
            // An unknown value keeps System: a hand-edited or newer settings
            // file must not be able to leave the app in a look it cannot draw.
            if (ls >> v) themeMode = ThemeModeFromString(v.c_str());
        }
        else if (kw == "takefmt") { int v; if (ls >> v) takeFormat = v; }
        else if (kw == "defrate")   { int v; if (ls >> v) defaultSampleRate = v; }
        else if (kw == "metsnd")    { int v; if (ls >> v) metronomeSound = v; }
        else if (kw == "metlvl")    { float v; if (ls >> v) metronomeLevel = v; }
        else if (kw == "uiscale")   { float v; if (ls >> v) uiScale = v; }
        else if (kw == "autosv")    { int v; if (ls >> v) autosaveSeconds = v; }
        else if (kw == "audprev")   { int v; if (ls >> v) previewNotes = (v != 0); }
        else if (kw == "projdir") {
            std::string rest;
            std::getline(ls, rest);
            if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
            projectDir = rest;
        }
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
    // Whatever the file said (or failed to say), what comes out of a load is
    // something the app can run with.
    Sanitize();
    return any;
}

} // namespace daw
