// Haiku DAW — application entry point (milestone 4b).
//
// Builds a Project, optionally seeds it with clips from WAV paths given on the
// command line (one clip per file, laid end to end on successive tracks), and
// opens the timeline window. No audio engine wired yet — that is M4d.
//
// Usage:  daw [file1.wav file2.wav ...]
//
// Haiku-only: links the Interface Kit. The model layer it drives is kit-free.

#include "model/Project.h"
#include "model/Commands.h"
#include "model/PeakCache.h"
#include "engine/WavSource.h"
#include "ui/MainWindow.h"

#include <Application.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace daw;

// Build a demo/session project. Each WAV path becomes its own audio track with
// a single clip starting at frame 0; length comes from the file's frame count.
static void SeedDemoTracks(Project& project, CommandStack& stack) {
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Audio 1"), project);
    // A MIDI track so the synth is reachable: click its lane to add notes.
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Synth"), project);
}

static void SeedProject(Project& project, CommandStack& stack,
                        const std::vector<std::string>& wavs) {
    if (wavs.empty()) {
        SeedDemoTracks(project, stack);   // nothing on the command line
        return;
    }

    int n = 1;
    for (const std::string& path : wavs) {
        WavSource src;
        if (!src.Open(path)) {
            std::fprintf(stderr, "daw: cannot open '%s' as WAV, skipping\n",
                         path.c_str());
            continue;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "Track %d", n++);
        auto add = std::make_unique<AddTrackCommand>(TrackType::Audio, name);
        AddTrackCommand* addPtr = add.get();
        stack.Execute(std::move(add), project);
        TrackId tid = addPtr->CreatedId();

        // lengthFrames is timeline (project-rate) frames; convert from the
        // source's own frame count by the rate ratio.
        const double srcRate = src.FrameRate();
        const double ratio = srcRate > 0 ? project.sampleRate / srcRate : 1.0;
        Clip clip;
        clip.startFrame   = 0;
        clip.lengthFrames = (Frame)llround(src.TotalFrames() * ratio);
        clip.sourceOffset = 0;
        clip.sourcePath   = path;
        stack.Execute(std::make_unique<AddClipCommand>(tid, clip), project);
    }

    // Every path failed to open: don't leave a blank window.
    if (project.Tracks().empty())
        SeedDemoTracks(project, stack);
}

// Build a min/max waveform envelope for every distinct clip source in the
// project. Done once, up front (on "import"), so the timeline never scans
// audio at paint time. Keyed by path so shared sources build only once.
static void BuildPeaks(const Project& project,
                       std::map<std::string, PeakCache>& out) {
    for (const Track& t : project.Tracks()) {
        for (const Clip& c : t.clips) {
            if (c.sourcePath.empty() || out.count(c.sourcePath))
                continue;
            WavSource src;
            if (!src.Open(c.sourcePath))
                continue;
            out[c.sourcePath].Build(src);
        }
    }
}

int main(int argc, char** argv) {
    BApplication app("application/x-vnd.DuskAudio-HaikuDAW");

    std::vector<std::string> wavs;
    for (int i = 1; i < argc; i++)
        wavs.push_back(argv[i]);

    // The project + command stack outlive the window (they are the session).
    static Project      project;
    static CommandStack stack;
    SeedProject(project, stack, wavs);

    static std::map<std::string, PeakCache> peaks;
    BuildPeaks(project, peaks);

    BRect frame(80, 80, 80 + 1000, 80 + 560);
    MainWindow* win = new MainWindow(frame, &project, &stack, &peaks);
    win->Show();

    app.Run();
    return 0;
}
