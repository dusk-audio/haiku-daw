// Lv2PresetStore — the DAW's own preset files.
//
// A preset is the pair (plugin state, control-port values): the first is what
// lilv's state API gives us (see IEffect::SaveState), the second is the same
// slot-ordered float vector EffectDesc.params carries. One file per preset, in
// one directory, keyed by the plugin URI inside the file rather than by the file
// name — so two plugins may both have a "Bright" preset and neither can shadow
// the other.
//
// Kit-free (POSIX dirent/stat only) so the format is host-tested without lilv,
// which is the point: the format is ours, and a wrong byte in it is a lost
// patch. lilv-discovered presets (lv2:Preset in a bundle) are read by Lv2Host
// and are NOT these files; the two lists are merged for the UI.
//
// Format (text, one record, base64 for the state so nothing in a plugin's
// document can break the line structure):
//
//   DAWPRESET 1
//   plugin <uri>
//   name <free text, the rest of the line>
//   state <base64 of the plugin's state document, or empty>
//   params <count> <v> <v> ...
#pragma once

#include "../model/Base64.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace daw {

// One preset, in the form both the file format and the UI want.
struct PresetEntry {
    std::string        name;
    std::string        state;   // the plugin's own document (decoded)
    std::vector<float> params;  // control-port values, slot order
};

// Where user presets live: DAW_LV2_PRESET_DIR when set (the tests, and an
// escape hatch), else $HOME/config/settings/HaikuDAW/presets — the same
// settings tree the app's other files use, which is ~/config on Haiku and
// simply unused elsewhere.
inline std::string PresetStoreDir() {
    if (const char* env = std::getenv("DAW_LV2_PRESET_DIR"))
        if (*env) return env;
    const char* home = std::getenv("HOME");
    if (!home || !*home) return std::string();
    return std::string(home) + "/config/settings/HaikuDAW/presets";
}

// A file name for `name` that cannot escape the directory or collide with an
// existing preset: anything outside [A-Za-z0-9._-] becomes '_', and a name
// already taken grows a "-2", "-3"... suffix. The NAME is not the file name —
// it lives inside the file — so mangling here loses nothing.
inline std::string PresetFileName(const std::string& dir,
                                  const std::string& name) {
    std::string base;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                     || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        base += ok ? c : '_';
    }
    if (base.empty()) base = "preset";
    if (base.size() > 64) base.resize(64);   // a name, not a document
    for (int n = 1; n < 10000; n++) {
        const std::string suffix = (n == 1) ? "" : "-" + std::to_string(n);
        const std::string path = dir + "/" + base + suffix + ".dawpreset";
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) return path;   // free
    }
    return dir + "/" + base + ".dawpreset";   // 10000 same-named presets: reuse
}

// Serialize one preset. `uri` is the plugin it applies to.
inline std::string SerializePreset(const std::string& uri,
                                   const PresetEntry& e) {
    std::ostringstream o;
    o << "DAWPRESET 1\n";
    o << "plugin " << uri << "\n";
    o << "name " << e.name << "\n";
    o << "state " << Base64Encode(e.state) << "\n";
    o << "params " << e.params.size();
    for (float v : e.params) o << " " << v;
    o << "\n";
    return o.str();
}

// Parse `text` as a preset for `uri` (a preset for another plugin is not
// `true`, and not an error either — see the listing). Rejects a missing/odd
// magic and a state that is not valid base64, so a torn or hand-edited file is
// skipped rather than loaded as a half-preset.
inline bool ParsePreset(const std::string& text, const std::string& uri,
                        PresetEntry* out) {
    if (!out) return false;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line)) return false;
    // Tolerate a trailing \r (a file that has been through a text-mode copy).
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (line != "DAWPRESET 1") return false;

    PresetEntry e;
    bool sawPlugin = false, sawName = false, sawState = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        const size_t sp = line.find(' ');
        const std::string kw = line.substr(0, sp);
        const std::string rest =
            (sp == std::string::npos) ? std::string() : line.substr(sp + 1);
        if (kw == "plugin") {
            if (rest != uri) return false;   // another plugin's preset
            sawPlugin = true;
        } else if (kw == "name") {
            e.name = rest;
            sawName = true;
        } else if (kw == "state") {
            if (!Base64Decode(rest, &e.state)) return false;
            sawState = true;
        } else if (kw == "params") {
            std::istringstream ps(rest);
            long long count = 0;
            if (!(ps >> count) || count < 0 || count > 100000) return false;
            for (long long i = 0; i < count; i++) {
                float v = 0.0f;
                if (!(ps >> v)) return false;
                e.params.push_back(v);
            }
        }
        // Unknown keys are skipped: a newer build's preset still loads here.
    }
    if (!sawPlugin || !sawName || !sawState || e.name.empty()) return false;
    *out = std::move(e);
    return true;
}

// Save one preset, creating the directory if it is not there. `dir` empty means
// "cannot be determined" and fails (PresetStoreDir() returned empty).
inline bool SavePresetToStore(const std::string& dir, const std::string& uri,
                              const PresetEntry& e) {
    if (dir.empty() || uri.empty() || e.name.empty()) return false;
    ::mkdir(dir.c_str(), 0755);   // EEXIST is the expected case
    const std::string path = PresetFileName(dir, e.name);
    const std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const std::string text = SerializePreset(uri, e);
    const bool wrote = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) { std::remove(tmp.c_str()); return false; }
    // Rename over the target, like ProjectIO::Save: an interrupted write leaves
    // the previous file, not half of a new one.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// Every preset in `dir` that applies to `uri`, by name (the file name is a
// mangled key, the name inside is the user's). An unreadable or malformed file
// is skipped, and so is another plugin's preset — a shared directory is not a
// corrupt one.
inline std::vector<PresetEntry> LoadPresetsFor(const std::string& dir,
                                               const std::string& uri) {
    std::vector<PresetEntry> out;
    if (dir.empty() || uri.empty()) return out;
    DIR* d = ::opendir(dir.c_str());
    if (!d) return out;
    while (dirent* ent = ::readdir(d)) {
        const std::string name = ent->d_name;
        const std::string suffix = ".dawpreset";
        if (name.size() <= suffix.size()
            || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::FILE* f = std::fopen((dir + "/" + name).c_str(), "rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        PresetEntry e;
        if (ParsePreset(text, uri, &e)) out.push_back(std::move(e));
    }
    ::closedir(d);
    return out;
}

} // namespace daw
