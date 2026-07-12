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
#include "engine/WavSource.h"
#include "ui/MainWindow.h"

#include <Application.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace daw;

// Build a demo/session project. Each WAV path becomes its own audio track with
// a single clip starting at frame 0; length comes from the file's frame count.
static void SeedProject(Project& project, CommandStack& stack,
                        const std::vector<std::string>& wavs) {
    if (wavs.empty()) {
        // No files: two empty tracks so the timeline has something to show.
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Track 1"), project);
        stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Track 2"), project);
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

        Clip clip;
        clip.startFrame   = 0;
        clip.lengthFrames = src.TotalFrames();
        clip.sourceOffset = 0;
        clip.sourcePath   = path;
        stack.Execute(std::make_unique<AddClipCommand>(tid, clip), project);
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

    BRect frame(80, 80, 80 + 1000, 80 + 560);
    MainWindow* win = new MainWindow(frame, &project);
    win->Show();

    app.Run();
    return 0;
}
