#include "ProjectResample.h"

#include "../engine/AudioFormats.h"
#include "../engine/IAudioSource.h"
#include "../engine/Resampler.h"
#include "../engine/WavSource.h"   // reading the file already at the target name
#include "../engine/WavWriter.h"
#include "TakeNames.h"   // NextFreeWavPath (a taken target name)

#include <cmath>
#include <cstdio>
#include <sys/stat.h>
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

    const std::string wanted = TargetPath(srcPath, targetRate);
    // The target name is derived from the source's, so it can already belong to
    // something. Two cases, and they want opposite answers:
    //   - our OWN earlier conversion of this same file (converting twice is
    //     idempotent, and the project's clips point at that one name), or
    //   - somebody else's file: a hand-made one, or the conversion of a
    //     DIFFERENT source that shares the stem (`take.wav` and `take.flac`
    //     both want `take-48k.wav`).
    // A file at the rate the conversion produces, of the length it would
    // produce, is the first case; anything else is left alone and the new copy
    // steps aside to the numbered convention the take paths use.
    std::string outPath = wanted;
    bool reuse = false;
    {
        struct stat st;
        if (::stat(wanted.c_str(), &st) == 0) {
            WavSource existing;
            const double expect = (double)src->TotalFrames() * targetRate / srcRate;
            if (existing.Open(wanted) && existing.IsValid()
                && std::fabs(existing.FrameRate() - targetRate) < 0.5
                && std::fabs((double)existing.TotalFrames() - expect)
                       <= 0.02 * expect + 2.0) {
                reuse = true;
            } else {
                const size_t slash = wanted.find_last_of('/');
                const std::string dir = slash == std::string::npos
                                      ? std::string() : wanted.substr(0, slash);
                std::string stem = slash == std::string::npos
                                 ? wanted : wanted.substr(slash + 1);
                if (stem.size() > 4) stem = stem.substr(0, stem.size() - 4);
                outPath = NextFreeWavPath(dir, stem);
                if (outPath.empty()) return fail("cannot find a free name");
            }
        }
    }
    if (reuse) return wanted;   // already converted; nothing to do
    WavWriter writer;
    // 32-bit float: a conversion keeps everything the source had, and this is
    // a working copy rather than a master. Exclusive: this never truncates a
    // file it did not create.
    if (!writer.OpenFormat(outPath, (int)std::llround(targetRate), 2, 32, true,
                           /*exclusive*/ true)) {
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
