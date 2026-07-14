// BfsAttr — read/write BFS extended attributes on a file.
//
// The native-Haiku superpower: audio files carry DAW metadata as filesystem
// attributes (DAW:bpm, DAW:key, DAW:duration), which a live BQuery can then
// search. This wraps BNode attribute I/O. Haiku-only (Storage Kit).
#pragma once

#include <string>

namespace daw {

// Attribute names (BFS "namespace:name" convention).
constexpr const char* kAttrBpm      = "DAW:bpm";       // float
constexpr const char* kAttrKey      = "DAW:key";       // string, e.g. "Am"
constexpr const char* kAttrDuration = "DAW:duration";  // float seconds

bool WriteAttrFloat (const char* path, const char* name, float v);
bool ReadAttrFloat  (const char* path, const char* name, float* out);
bool WriteAttrString(const char* path, const char* name, const std::string& s);
bool ReadAttrString (const char* path, const char* name, std::string* out);

// Ensure the volume has indexes for the DAW attributes so BQuery can search
// them. Safe to call repeatedly (existing indexes are left alone).
void EnsureDawIndexes(const char* pathOnVolume);

} // namespace daw
