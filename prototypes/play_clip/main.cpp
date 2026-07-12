// play_clip — Milestone 2 driver.
//
// Wires the model layer to the engine: builds a Project (via the command
// stack, dogfooding milestone 1), places one audio clip pointing at a WAV
// on the command line, then plays it through the Media Kit engine.
//
// Usage:  play_clip <audiofile> [gain]
//   gain defaults to 1.0 (linear).
//
// The project sample rate is set to the file's rate so milestone 2 needs
// no resampler: the output opens at the file rate, pitch stays correct.

#include "../../src/model/Commands.h"
#include "../../src/engine/Engine.h"
#include "../../src/engine/WavSource.h"

#include <OS.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace daw;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <audiofile> [gain]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const float gain = (argc >= 3) ? std::atof(argv[2]) : 1.0f;

    // Probe the file up front for its length + rate so the clip and the
    // output are sized correctly.
    WavSource probe;
    if (probe.Open(path) != B_OK) {
        std::fprintf(stderr, "could not open '%s'\n", path);
        return 1;
    }
    const int64_t totalFrames = probe.TotalFrames();
    const double  fileRate    = probe.FrameRate();
    std::printf("File: %s\n  rate = %.1f Hz, channels = %d, frames = %lld "
                "(%.2f s)\n",
                path, fileRate, probe.SourceChannels(),
                (long long)totalFrames, totalFrames / fileRate);

    // --- Build the project through the command stack (milestone 1) -----
    Project project;
    project.sampleRate = (fileRate > 0) ? fileRate : 48000.0;
    CommandStack stack;

    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio,
                                                    "Track 1"), project);
    const TrackId tid = project.Tracks().front().id;

    if (gain != 1.0f)
        stack.Execute(std::make_unique<SetTrackGainCommand>(tid, gain), project);

    Clip clip;
    clip.startFrame   = 0;
    clip.lengthFrames = totalFrames;
    clip.sourcePath   = path;
    stack.Execute(std::make_unique<AddClipCommand>(tid, clip), project);

    // --- Play through the engine (milestone 2) -------------------------
    Engine engine;
    if (engine.Load(project) != B_OK) {
        std::fprintf(stderr, "engine load failed\n");
        return 1;
    }
    std::printf("Output rate: %.1f Hz. Playing (gain %.2f)...\n",
                engine.OutputRate(), gain);

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
