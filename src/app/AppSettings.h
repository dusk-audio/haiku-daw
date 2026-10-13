// AppSettings — persisted application preferences + window layout.
//
// Plain data + text (de)serialization, kit-free so it round-trips in host
// tests. The platform-specific load/save (locating ~/config/settings) lives in
// MainWindow. One `key value` line each; unknown keys are ignored so the format
// can grow.
#pragma once

#include "ThemePalette.h"   // ThemeMode

#include <string>
#include <vector>

namespace daw {

// The values the Preferences window (M3.5) offers for the settings that are a
// choice, one table each so the window's menus and what a load accepts cannot
// drift apart: Sanitize() snaps a loaded value to one of these. The tables are
// also what a functional test reads to drive a menu by index.
inline constexpr int kBufferFrameChoices[]    = { 128, 256, 512, 1024, 2048 };
inline constexpr int kBufferFrameChoiceCount = 5;
inline constexpr int kSampleRateChoices[]     = { 44100, 48000, 96000 };
inline constexpr int kSampleRateChoiceCount  = 3;
inline constexpr int kCountInChoices[]        = { 0, 1, 2, 4 };
inline constexpr int kCountInChoiceCount     = 4;
inline constexpr int kAutosaveChoices[]       = { 0, 30, 60, 300, 600 };  // s
inline constexpr int kAutosaveChoiceCount    = 5;
// The metronome sounds Metronome::Render knows, by index (M3.5).
inline constexpr int kMetronomeSoundCount    = 3;
// UI scales the menu offers; 0 is "follow the user's font size" (Theme.h).
inline constexpr float kUiScaleChoices[]      = { 0.0f, 1.0f, 1.25f, 1.5f };
inline constexpr int   kUiScaleChoiceCount   = 4;

struct AppSettings {
    int   bufferFrames = 512;
    int   countInBars  = 0;
    bool  metronome    = false;
    bool  monitorInput = false;
    std::string lastDir;                 // last Open/Save/Import directory
    float winL = 80, winT = 80, winR = 1280, winB = 820;   // window frame

    // Take sample format for recording (M5): 0 = 16-bit PCM, 1 = 24-bit PCM
    // (default), 2 = 32-bit float. The Preferences window belongs to another
    // package -- this is the key it sets; the recorder reads it.
    int   takeFormat = 1;

    // The last few projects saved or opened, most recent first (the File
    // menu's Open Recent). At most kMaxRecent; plain paths, one `recent` line
    // each so a path with spaces survives.
    // The M1.4 panes: the inspector column (width and whether it shows) and
    // the docked bottom pane (the editor/browser tabs) the same.
    bool  inspectorVisible = true;
    float inspectorWidth   = 190.0f;
    bool  bottomVisible    = false;
    float bottomHeight     = 260.0f;
    // Which dock page was showing (T2): 0 Editor, 1 Samples, 2 Plugins.
    int   dockPage         = 0;
    // Whether the arrangement shows the marker lane (M2.6): a view
    // preference like the panes above, not project data.
    bool  markerLaneVisible = false;

    // How the app looks: the user's own colours and the stock Haiku look
    // (System), or the DAW's dark palette in the DAW's windows only (Dark).
    // System is the default; the app never writes a system colour (T1).
    ThemeMode themeMode = ThemeMode::System;

    // --- the Preferences window (M3.5) ------------------------------------
    // The rate a NEW project starts at (the rate of the running session still
    // comes from the project itself). One of kSampleRateChoices.
    int   defaultSampleRate = 48000;
    // The metronome click: its sound (an index into what Metronome knows, see
    // kMetronomeSoundCount) and its level (0..1, the click's own gain).
    int   metronomeSound    = 0;
    float metronomeLevel    = 0.30f;
    // The UI scale: 0 = follow the user's font size (Theme.h's default), a
    // positive value forces it through SetThemeScaleOverride.
    float uiScale           = 0.0f;
    // The directory a New/Save panel starts in when the session has no
    // remembered lastDir yet. Plain path; empty = the panel's own default.
    std::string projectDir;
    // The crash-recovery interval in seconds; 0 turns autosave off. One of
    // kAutosaveChoices (a loaded value is clamped, not snapped).
    int   autosaveSeconds   = 30;

    std::vector<std::string> recentProjects;
    static constexpr size_t kMaxRecent = 10;

    // Move `path` to the front of `recent`: deduplicated, most-recent-first,
    // truncated to kMaxRecent.
    static void RememberRecent(std::vector<std::string>& recent,
                               const std::string& path);

    // The export dialog's last choices, so a repeat bounce does not have to be
    // re-specified. 0 sample rate = the project's own; range 0 = whole project,
    // 1 = the loop range; stems 0 = mixdown, 1 = stems.
    // exportContainer is AudioFileFormat's ordinal (0 WAV, 1 AIFF, 2 FLAC,
    // 3 Ogg Vorbis) as an int, so this stays a plain settings struct with no
    // include of the engine headers; a value this build cannot write is
    // ignored on load (see MainWindow::LoadSettings).
    int   exportContainer = 0;
    int   exportBitDepth  = 16;
    float exportVorbisQuality = 0.5f;
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

    // Snap every preference to a value the app can actually run with, and run
    // it at the end of Deserialize. A hand-edited, truncated or newer settings
    // file must not be able to leave the app with a buffer of 0 frames (a
    // division in the audio path), a negative metronome level, a scale it
    // cannot draw at, or a metronome sound that is not one it knows.
    void Sanitize();
};

} // namespace daw
