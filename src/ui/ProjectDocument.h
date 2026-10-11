// ProjectDocument — what the session IS on disk: the file it came from, the
// recovery copy, and the recent-project list.
//
// M1.1's first slice out of MainWindow: this is the state and the bookkeeping
// that every save/open/recover/new path juggles, extracted so the four flows
// agree by construction and so later work (the theme, the docking rework) has
// less of one 3.2k-line file to collide in. MainWindow keeps what a window
// owns: the prompts, the file panels, the menus and the timeline.
#pragma once

#include "../app/AppSettings.h"   // ReadSettings returns one
#include "../model/Command.h"

#include <Path.h>

#include <string>
#include <vector>

namespace daw {

class Project;

class ProjectDocument {
public:
    ProjectDocument(Project& project, CommandStack& stack);

    // --- the project's file ------------------------------------------------
    const std::string& Path() const { return fPath; }
    bool HasPath() const { return !fPath.empty(); }

    // The window title: "*name — Haiku DAW" while dirty, the base name (or
    // "Untitled") otherwise.
    std::string Title() const;

    // The project's display name (base name, extension dropped; "Untitled"
    // when there is no path). The recent menu labels its items with it.
    static std::string DisplayName(const std::string& path);

    // Record what just happened, so every flow goes through one place:
    void NoteSaved(const std::string& path);     // path + clean + no recovery
    void NoteLoaded(const std::string& path);    // path + clean + recent
    void NoteRecovered();                        // unsaved, nameless work
    void NoteNew();                              // File > New: no path, clean

    // --- the recovery copy -------------------------------------------------
    // ~/config/settings/HaikuDAW/recovery.dawproj (empty when the settings
    // directory cannot be found). Removed only on a clean save, an explicit
    // Discard, or a clean quit — a Cancel keeps it.
    bool RecoveryPath(BPath& out) const;
    void RemoveRecoveryFile() const;

    // Where settings live (the window's own preferences use it too).
    static bool SettingsPath(BPath& out);

    // The settings file as data (defaults when it is missing or garbage).
    // DawApplication reads the theme mode through this before any window
    // exists; MainWindow reads the rest on top of its own defaults.
    static AppSettings ReadSettings();

    // --- File > Open Recent ------------------------------------------------
    const std::vector<std::string>& Recent() const { return fRecent; }
    void Remember(const std::string& path);   // front, deduplicated, capped
    void Forget(const std::string& path);
    void SetRecent(std::vector<std::string> recent) { fRecent = std::move(recent); }

private:
    Project&            fProject;
    CommandStack&       fStack;
    std::string         fPath;
    std::vector<std::string> fRecent;
};

} // namespace daw
