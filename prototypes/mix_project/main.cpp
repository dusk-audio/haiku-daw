// mix_project — Milestone 3 driver: multitrack mixing.
//
// Loads several audio files, one per track, and plays them mixed through
// the engine with per-track gain and equal-power pan. All clips start at
// frame 0 so they overlap and you hear the sum.
//
// Usage:  mix_project <spec> [<spec> ...]
//   spec = path[@gain[:pan]]
//     gain: linear, default 1.0
//     pan : -1 (left) .. 0 (center) .. +1 (right), default 0
//
// Examples:
//   mix_project drums.wav bass.wav
//   mix_project drums.wav@0.8 gtr.wav@0.6:-0.5 vox.wav@0.7:0.5

#include "../../src/model/Commands.h"
#include "../../src/engine/Engine.h"
#include "../../src/engine/WavSource.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace daw;

struct Spec { std::string path; float gain = 1.0f; float pan = 0.0f; };

static Spec ParseSpec(const std::string& arg) {
    Spec s;
    std::string a = arg;
    const size_t at = a.find('@');
    if (at != std::string::npos) {
        std::string rest = a.substr(at + 1);
        a = a.substr(0, at);
        const size_t col = rest.find(':');
        if (col != std::string::npos) {
            s.gain = std::atof(rest.substr(0, col).c_str());
            s.pan  = std::atof(rest.substr(col + 1).c_str());
        } else {
            s.gain = std::atof(rest.c_str());
        }
    }
    s.path = a;
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <path[@gain[:pan]]> ...\n", argv[0]);
        return 1;
    }

    Project project;
    CommandStack stack;
    double projectRate = 0.0;

    for (int i = 1; i < argc; i++) {
        Spec spec = ParseSpec(argv[i]);

        // Probe length + rate with our own reader.
        WavSource probe;
        if (!probe.Open(spec.path)) {
            std::fprintf(stderr, "skip (open failed): %s\n", spec.path.c_str());
            continue;
        }
        const int64_t frames = probe.TotalFrames();
        const double  rate   = probe.FrameRate();
        if (projectRate == 0.0) projectRate = rate;

        std::printf("track %d: %s  gain=%.2f pan=%+.2f  %.1f Hz  %.2f s\n",
                    i, spec.path.c_str(), spec.gain, spec.pan,
                    rate, frames / rate);

        // Build the track + clip through the command stack.
        auto add = std::make_unique<AddTrackCommand>(TrackType::Audio,
                                                     spec.path);
        stack.Execute(std::move(add), project);
        const TrackId tid = project.Tracks().back().id;

        if (spec.gain != 1.0f)
            stack.Execute(std::make_unique<SetTrackGainCommand>(tid, spec.gain),
                          project);
        if (spec.pan != 0.0f)
            stack.Execute(std::make_unique<SetTrackPanCommand>(tid, spec.pan),
                          project);

        Clip clip;
        clip.startFrame   = 0;
        clip.lengthFrames = frames;
        clip.sourcePath   = spec.path;
        stack.Execute(std::make_unique<AddClipCommand>(tid, clip), project);
    }

    if (project.Tracks().empty()) {
        std::fprintf(stderr, "no playable tracks\n");
        return 1;
    }
    project.sampleRate = (projectRate > 0) ? projectRate : 48000.0;

    Engine engine;
    if (engine.Load(project) != B_OK) {
        std::fprintf(stderr, "engine load failed\n");
        return 1;
    }
    std::printf("Mixing %zu tracks at %.1f Hz...\n",
                project.Tracks().size(), engine.OutputRate());

    engine.Start();
    while (!engine.IsFinished()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("\r  playhead: %.2f s   ",
                    engine.Playhead() / project.sampleRate);
        std::fflush(stdout);
    }
    engine.Stop();
    std::printf("\nDone.\n");
    return 0;
}
