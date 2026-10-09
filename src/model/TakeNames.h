// Choosing a free output file name for recorded/rendered audio.
//
// Why this exists: the take writer used to be named from a counter that
// restarted at 0 every session, and WavWriter opened with trunc -- so
// recording again after reopening a project silently overwrote take-1.wav,
// a file the project's clips still referenced. Scanning for the first free
// name makes a reopened session land on take-2.wav, and WavWriter's
// exclusive open (see WavWriter.h) is the backstop if two picks ever race.
//
// Kit-free (POSIX stat only) so the choice is host-testable.
#pragma once

#include <cstdio>
#include <string>
#include <sys/stat.h>

namespace daw {

// The first free "<dir>/<prefix>-<n>.wav" (n from `first`), where free means
// the path does not exist. An empty dir means the working directory. Gaps are
// reused: a directory holding only "-7.wav" yields "-1.wav".
inline std::string NextFreeWavPath(const std::string& dir,
                                   const std::string& prefix, int first = 1) {
    const std::string base = (dir.empty() ? std::string() : dir + "/") + prefix;
    for (int n = first; n < 1000000; ++n) {
        char suffix[32];
        std::snprintf(suffix, sizeof(suffix), "-%d.wav", n);
        const std::string path = base + suffix;
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
            return path;   // free (missing is the only answer we act on)
    }
    return std::string();   // a directory with a million takes: give up loudly
}

} // namespace daw
