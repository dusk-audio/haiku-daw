// AppSettings — persisted application preferences + window layout.
//
// Plain data + text (de)serialization, kit-free so it round-trips in host
// tests. The platform-specific load/save (locating ~/config/settings) lives in
// MainWindow. One `key value` line each; unknown keys are ignored so the format
// can grow.
#pragma once

#include <string>

namespace daw {

struct AppSettings {
    int   bufferFrames = 512;
    int   countInBars  = 0;
    bool  metronome    = false;
    bool  monitorInput = false;
    std::string lastDir;                 // last Open/Save/Import directory
    float winL = 80, winT = 80, winR = 1280, winB = 820;   // window frame

    // Serialize to / parse from the settings text. Deserialize leaves defaults
    // for missing keys and returns false only on empty/garbage input.
    std::string Serialize() const;
    bool Deserialize(const std::string& text);
};

} // namespace daw
