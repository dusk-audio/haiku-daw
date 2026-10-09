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

    // The export dialog's last choices, so a repeat bounce does not have to be
    // re-specified. 0 sample rate = the project's own; range 0 = whole project,
    // 1 = the loop range; stems 0 = mixdown, 1 = stems.
    int   exportBitDepth  = 16;
    bool  exportDither    = true;
    int   exportSampleRate = 0;
    bool  exportNormalize = false;
    float exportTargetLufs = -14.0f;
    float exportTruePeakCeil = -1.0f;
    bool  exportLimiter   = false;
    int   exportRange     = 0;
    int   exportStems     = 0;

    // Serialize to / parse from the settings text. Deserialize leaves defaults
    // for missing keys and returns false only on empty/garbage input.
    std::string Serialize() const;
    bool Deserialize(const std::string& text);
};

} // namespace daw
