// SfzParser — SFZ text to a LoadedInstrument.
//
// Parses the subset of SFZ that real instrument libraries actually use, plus
// everything the SF2 converter emits. SFZ is a flat opcode language: headers
// (<control>, <global>, <group>, <region>) open scopes, and a <region>
// inherits every opcode set by the scopes enclosing it. Unknown opcodes are
// ignored, which is the format's own convention — files routinely carry
// player-specific extensions.
//
// Kit-free (std C++ only), host-testable. Sample loading goes through
// LoadWavToMemory; each distinct sample file is decoded once and shared by
// every region referencing it.
#pragma once

#include "SampleBank.h"

#include <string>

namespace daw {

// Parse an .sfz file and populate `out` with its regions and their decoded
// samples. Calls FinalizeInstrument before returning. Returns false on a
// missing/unreadable file, setting *error if non-null. A file that parses but
// whose samples are all missing returns true with no regions — the caller
// decides whether that is an error.
// `warning` (optional) receives a PARTIAL-SUCCESS note: the file loaded and the
// regions present are playable, but something was dropped — most often the
// instrument hitting kMaxInstrumentBytes. Distinct from `error`, which means
// nothing usable was produced and the return is false.
bool LoadSfz(const std::string& path, LoadedInstrument* out, std::string* error,
             std::string* warning = nullptr);

// Parse SFZ source text directly, resolving relative sample= paths against
// `baseDir` (no trailing slash needed). Exposed for tests, which build source
// inline rather than on disk.
bool ParseSfzText(const std::string& text, const std::string& baseDir,
                  LoadedInstrument* out, std::string* error,
                  std::string* warning = nullptr);

} // namespace daw
