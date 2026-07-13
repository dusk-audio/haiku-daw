// record_clip — M5 driver: capture the system audio input to a WAV file.
//
//   record_clip <out.wav> [seconds]
//
// Proves the capture path end to end (BMediaRecorder -> ring -> disk) before
// wiring recording into the UI. Needs the desktop session (media_server).

#include "../../src/engine/Recorder.h"

#include <Application.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

using namespace daw;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.wav> [seconds]\n", argv[0]);
        return 1;
    }
    const char* path = argv[1];
    const double seconds = (argc > 2) ? std::atof(argv[2]) : 3.0;

    // A BApplication is needed for the Media Kit roster/messaging to work.
    BApplication app("application/x-vnd.DuskAudio-record_clip");

    Recorder rec;
    if (rec.Start(path) != B_OK) {
        std::fprintf(stderr, "record_clip: failed to start capture\n");
        return 1;
    }
    std::printf("Recording %.1f s to %s ...\n", seconds, path);
    usleep((useconds_t)(seconds * 1e6));
    rec.Stop();

    std::printf("Done: %lld frames @ %.0f Hz, %d ch -> %s\n",
                (long long)rec.FramesWritten(), rec.SampleRate(),
                rec.Channels(), path);
    return 0;
}
