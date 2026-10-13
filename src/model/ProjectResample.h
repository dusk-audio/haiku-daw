// ProjectResample — writing a media file at another sample rate.
//
// The engine plays any source rate at the right pitch (every stream resamples
// on its disk thread), so this is housekeeping, not a requirement of playback:
// it is what Project Settings' "Resample the media" offer does (M3.6), giving
// a project whose media all matches its rate for interop, archiving and anyone
// opening the folder without the DAW. The original file is never touched.
//
// Kit-free: the format factory, the linear resampler and the WAV writer are all
// host-testable, so a round trip through this is a test rather than a click.
#pragma once

#include <string>

namespace daw {

// Write interleaved-stereo 32-bit float WAV `dir/of/src/<stem>-<rate>.wav`
// beside `srcPath` (an existing file of that name is replaced -- it is this
// function's own output from an earlier conversion) and return the new path.
// Returns "" and (when given) sets *error when the file cannot be read or the
// new one cannot be written; the caller then leaves its clip on the old file.
//
// `targetRate` must be > 0 and the file's own rate must be known; a file
// already at `targetRate` is still written (one rule, no surprises).
std::string ResampleMediaFile(const std::string& srcPath, double targetRate,
                              std::string* error = nullptr);

} // namespace daw
