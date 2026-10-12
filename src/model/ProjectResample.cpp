#include "ProjectResample.h"

#include "../engine/AudioFormats.h"
#include "../engine/IAudioSource.h"
#include "../engine/Resampler.h"
#include "../engine/WavWriter.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace daw {

namespace {

// Where the new file goes, and what it is called: beside the original, with the
// rate in the name so the folder says which copy is which (the same idea as
// the take/`-2` naming the render paths use).
std::string TargetPath(const std::string& srcPath, double targetRate) {
    const size_t slash = srcPath.find_last_of('/');
    const std::string dir  = slash == std::string::npos
                           ? std::string() : srcPath.substr(0, slash + 1);
    std::string stem = slash == std::string::npos
                     ? srcPath : srcPath.substr(slash + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "-%dk.wav",
                  (int)std::llround(targetRate / 1000.0));
    return dir + stem + suffix;
}

} // namespace

std::string ResampleMediaFile(const std::string& srcPath, double targetRate,
                              std::string* error) {
    auto fail = [&](const char* what) -> std::string {
        if (error != nullptr) *error = what;
        return std::string();
    };
    if (srcPath.empty())            return fail("no file");
    if (!(targetRate > 0.0))        return fail("no target rate");

    std::string openError;
    std::unique_ptr<IAudioSource> src = OpenAudioSource(srcPath, &openError);
    if (!src || !src->IsValid()) {
        return fail(openError.empty() ? "unreadable file" : openError.c_str());
    }
    const double srcRate = (double)src->FrameRate();
    if (!(srcRate > 0.0)) return fail("the file reports no sample rate");

    const std::string outPath = TargetPath(srcPath, targetRate);
    WavWriter writer;
    // 32-bit float: a conversion keeps everything the source had, and this is
    // a working copy rather than a master.
    if (!writer.OpenFormat(outPath, (int)std::llround(targetRate), 2, 32, true)) {
        return fail("cannot write the new file");
    }

    Resampler resampler(srcRate, targetRate, 2);
    std::vector<float> out;
    out.reserve(16384 * 2);
    const float* in = nullptr;
    size_t inFrames = 0;
    while (src->ReadChunk(&in, &inFrames)) {
        if (inFrames == 0) continue;
        out.clear();
        resampler.Process(in, inFrames, out);
        if (!out.empty()
            && !writer.WriteFloat(out.data(), out.size(), false)) {
            writer.Close();
            return fail("the write failed (disk full?)");
        }
    }
    if (!writer.Close()) return fail("the new file could not be finalized");
    return outPath;
}

} // namespace daw
