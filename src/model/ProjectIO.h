// ProjectIO — save/load a Project to a simple text file.
//
// The model is the single source of truth, so persistence is just a
// serializer over it. The format is a plain line-based text ("DAW 1" header),
// which is kit-free, diff-friendly, and host-testable — no BMessage/BFile, no
// JSON dependency. Strings (track name, clip path) are the last field on their
// line, double-quoted; they must not contain double-quotes or newlines (v1
// limitation).
//
// Load() clears the given Project and rebuilds it, restoring ids and bumping
// the id allocators so later edits don't collide.
#pragma once

#include "Project.h"

#include <string>

namespace daw {

class ProjectIO {
public:
    static bool Save(const Project& project, const std::string& path);
    static bool Load(Project& project, const std::string& path);
};

} // namespace daw
