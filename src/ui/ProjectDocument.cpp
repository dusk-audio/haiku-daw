#include "ProjectDocument.h"

#include "../app/AppSettings.h"
#include "../model/Project.h"

#include <File.h>
#include <FindDirectory.h>

#include <cstdio>
#include <sys/stat.h>

namespace daw {

// The project's display name: the file's base name with its extension
// dropped, or "Untitled" before the first save.
std::string ProjectDocument::DisplayName(const std::string& path) {
    if (path.empty()) return "Untitled";
    std::string name = path;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

ProjectDocument::ProjectDocument(Project& project, CommandStack& stack)
    : fProject(project), fStack(stack) {}

std::string ProjectDocument::Title() const {
    std::string title;
    if (fStack.IsDirty()) title = "*";
    title += DisplayName(fPath);
    title += " — Haiku DAW";
    return title;
}

void ProjectDocument::NoteSaved(const std::string& path) {
    fPath = path;
    fStack.MarkSaved();
    // The file on disk is this state now, so the recovery copy (which exists
    // to rescue unsaved work) is done. A failed save never reaches here.
    RemoveRecoveryFile();
    Remember(path);
}

void ProjectDocument::NoteLoaded(const std::string& path) {
    fPath = path;
    fStack.MarkSaved();   // the loaded file IS the saved state
    Remember(path);
}

void ProjectDocument::NoteNew() {
    // File > New: a fresh project that exists nowhere on disk yet, and is
    // clean -- there is nothing the user could lose.
    fPath.clear();
    fStack.MarkSaved();
}

void ProjectDocument::NoteRecovered() {
    // A recovered session is UNSAVED, nameless work: the recovery file is a
    // rescue copy, not the project. Treating it as an opened document made the
    // next Quit (clean -> no prompt) delete the only copy, and a Cmd-S save it
    // to the recovery path and then delete that same path.
    fPath.clear();
    fStack.MarkUnsaved();
}

bool ProjectDocument::SettingsPath(BPath& out) {
    BPath p;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK) return false;
    p.Append("HaikuDAW");
    mkdir(p.Path(), 0755);   // ignore EEXIST
    p.Append("settings");
    out = p;
    return true;
}

// The settings as data. DawApplication reads the theme mode through this
// before any window exists (the control look is process-wide, so it has to be
// chosen before the first window draws), and MainWindow reads the rest on top
// of its own defaults.
AppSettings ProjectDocument::ReadSettings() {
    AppSettings s;
    BPath p;
    if (!SettingsPath(p)) return s;
    BFile f(p.Path(), B_READ_ONLY);
    if (f.InitCheck() != B_OK) return s;
    off_t size = 0;
    if (f.GetSize(&size) != B_OK || size <= 0 || size > 65536) return s;
    std::string text;
    text.resize((size_t)size);
    if (f.Read(&text[0], (size_t)size) != (ssize_t)size) return s;
    s.Deserialize(text);   // a garbage file keeps the defaults
    return s;
}

bool ProjectDocument::RecoveryPath(BPath& out) const {
    BPath p;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &p) != B_OK) return false;
    p.Append("HaikuDAW");
    mkdir(p.Path(), 0755);
    p.Append("recovery.dawproj");
    out = p;
    return true;
}

void ProjectDocument::RemoveRecoveryFile() const {
    BPath p;
    if (RecoveryPath(p)) std::remove(p.Path());
}

void ProjectDocument::Remember(const std::string& path) {
    AppSettings::RememberRecent(fRecent, path);
}

void ProjectDocument::Forget(const std::string& path) {
    for (auto it = fRecent.begin(); it != fRecent.end(); ++it)
        if (*it == path) {
            fRecent.erase(it);
            return;
        }
}

} // namespace daw
