// ProjectTemplates — the New Project dialog's templates (plan M3.7).
//
// A template IS an ordinary `.dawproj` file, so "new from a template" is
// `ProjectIO::Load` of that file into the fresh session and there is no second
// project format to keep in step. This is the discovery half: which directory
// is used, what it holds, and what each row says.
//
// Kit-free (plain paths, the filesystem, `dirent`), so the rules that matter --
// which candidate directory wins, what a MISSING directory does, the row order,
// and that a broken file is reported rather than crashing the dialog -- are
// host tests rather than clicks.
#pragma once

#include <string>
#include <vector>

namespace daw {

class Project;

struct TemplateInfo {
    std::string id;           // "empty"/"band"/"producer", or the file's stem
    std::string name;         // the row's label
    std::string description;  // one line about what it builds
    std::string path;         // the .dawproj file ("" = the built-in Empty)
};

// The first candidate that exists AND holds at least one `.dawproj`; "" when
// none does. A candidate may name the directory or the `.dawproj` itself.
// Nothing here throws or fails: a missing directory is a normal answer, and the
// caller falls back to its built-in Empty row.
std::string FindTemplatesDir(const std::vector<std::string>& candidates);

// The templates in `dir`: the known ones first (empty, band, producer, in that
// order whatever order the directory lists them in), then every other
// `.dawproj` alphabetically, so dropping a file into data/templates is enough
// to offer it. A missing/unreadable directory yields NO rows -- not an error.
std::vector<TemplateInfo> ScanTemplates(const std::string& dir);

// Where the app looks, in order, given its own binary's directory and the
// installed data directory ("" when the platform call failed). The environment
// variable `DAW_TEMPLATES_DIR` wins when it is set, which is how the functional
// suite and a developer point the app at a checkout.
std::vector<std::string> TemplateCandidates(const std::string& appDir,
                                            const std::string& dataDir);

// The built-in Empty row (no file behind it): what the dialog offers when the
// template directory is missing, so the dialog is never a dead end.
TemplateInfo EmptyTemplate();

// Read a template's project. False when the file cannot be read or parsed; the
// caller then leaves the session alone and says so.
bool LoadTemplate(const TemplateInfo& info, Project& out);

} // namespace daw
