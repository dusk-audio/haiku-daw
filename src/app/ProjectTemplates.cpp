#include "ProjectTemplates.h"

#include "../model/ProjectIO.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace daw {

namespace {

constexpr const char* kExtension = ".dawproj";

// The shipped templates, in the order the New Project dialog shows them, with
// what each one is. A file whose stem is not one of these still gets a row (see
// ScanTemplates); it just cannot have a hand-written description.
struct KnownTemplate {
    const char* id;
    const char* name;
    const char* description;
};

const KnownTemplate kKnown[] = {
    { "empty", "Empty", "Nothing at all: a fresh session with no tracks." },
    { "band", "Band",
      "8 audio tracks, ready to record, with the drums on their own bus." },
    { "producer", "Producer",
      "MIDI lead and pads, audio tracks, and a reverb bus the vocal and the "
      "drums already send to." },
};

int KnownIndex(const std::string& id) {
    for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); i++)
        if (id == kKnown[i].id) return (int)i;
    return -1;
}

std::string StemOf(const std::string& file) {
    const std::string ext = kExtension;
    if (file.size() > ext.size()
        && file.compare(file.size() - ext.size(), ext.size(), ext) == 0)
        return file.substr(0, file.size() - ext.size());
    return file;
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir[dir.size() - 1] == '/') return dir + name;
    return dir + "/" + name;
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

bool HasProjectExtension(const std::string& name) {
    const std::string ext = kExtension;
    return name.size() > ext.size()
        && name.compare(name.size() - ext.size(), ext.size(), ext) == 0;
}

// The `.dawproj` files directly in `dir`, sorted. Empty when the directory
// cannot be opened -- that is the "no templates" answer, not an error.
std::vector<std::string> ProjectFilesIn(const std::string& dir) {
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return files;
    while (struct dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (!HasProjectExtension(name)) continue;
        if (!IsFile(JoinPath(dir, name))) continue;
        files.push_back(name);
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    return files;
}

TemplateInfo Describe(const std::string& dir, const std::string& file) {
    const std::string id = StemOf(file);
    TemplateInfo info;
    info.id = id;
    info.path = JoinPath(dir, file);
    const int known = KnownIndex(id);
    if (known >= 0) {
        info.name = kKnown[known].name;
        info.description = kKnown[known].description;
    } else {
        // A file someone dropped in: the row is its name, and the description
        // says where it came from rather than pretending to know what it is.
        info.name = id;
        info.description = "A project in the templates folder.";
    }
    return info;
}

} // namespace

TemplateInfo EmptyTemplate() {
    const int known = KnownIndex("empty");
    TemplateInfo info;
    info.id = "empty";
    info.name = kKnown[known].name;
    info.description = kKnown[known].description;
    return info;   // no path: the caller builds the empty project itself
}

std::string FindTemplatesDir(const std::vector<std::string>& candidates) {
    for (const std::string& c : candidates) {
        if (c.empty()) continue;
        // A candidate may be the directory or a project file inside it (an
        // override that names one file is still a usable directory).
        if (IsFile(c)) return c.substr(0, c.find_last_of('/'));
        if (!IsDirectory(c)) continue;
        if (!ProjectFilesIn(c).empty()) return c;
    }
    return std::string();
}

std::vector<TemplateInfo> ScanTemplates(const std::string& dir) {
    std::vector<TemplateInfo> out;
    if (dir.empty()) return out;
    const std::vector<std::string> files = ProjectFilesIn(dir);
    if (files.empty()) return out;

    // The known templates first, in their own order, then everything else
    // alphabetically. Each file is listed once (a duplicate stem, e.g. two
    // files differing only in case, is not a thing on a case-sensitive fs).
    std::vector<bool> used(files.size(), false);
    for (size_t k = 0; k < sizeof(kKnown) / sizeof(kKnown[0]); k++) {
        for (size_t i = 0; i < files.size(); i++) {
            if (used[i] || StemOf(files[i]) != kKnown[k].id) continue;
            used[i] = true;
            out.push_back(Describe(dir, files[i]));
        }
    }
    for (size_t i = 0; i < files.size(); i++) {
        if (used[i]) continue;
        out.push_back(Describe(dir, files[i]));
    }
    return out;
}

std::vector<std::string> TemplateCandidates(const std::string& appDir,
                                            const std::string& dataDir) {
    std::vector<std::string> candidates;
    // 1. The environment: tests and a developer working from a source tree.
    if (const char* env = std::getenv("DAW_TEMPLATES_DIR"))
        if (env[0] != '\0') candidates.push_back(env);
    // 2. A build tree: the binary sits in <build>/ and the copy rule puts the
    //    templates in <build>/data/templates (and <build>/../data for an
    //    in-tree absolute binary path).
    if (!appDir.empty()) {
        candidates.push_back(JoinPath(appDir, "data/templates"));
        candidates.push_back(JoinPath(appDir, "../data/templates"));
    }
    // 3. An installed app: the package puts them in the data directory
    //    (data/haiku-daw/templates).
    if (!dataDir.empty())
        candidates.push_back(JoinPath(dataDir, "haiku-daw/templates"));
    return candidates;
}

bool LoadTemplate(const TemplateInfo& info, Project& out) {
    if (info.path.empty()) return false;
    // ProjectIO::Load parses into a temp project and commits only on a clean
    // parse, so a corrupt template cannot leave a half-built session behind.
    return ProjectIO::Load(out, info.path);
}

} // namespace daw
