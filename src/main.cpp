// Haiku DAW — application entry point.
//
// A bare launch starts a demo session. A `.dawproj` handed to the app -- a
// double-click in Tracker, `open`, a drag onto the Deskbar entry, or a path on
// the command line -- is opened through the same message the File menu's open
// panel posts, so the unsaved-changes prompt is honoured and there is exactly
// one open path (MainWindow's LoadFrom).
//
// The project type itself is registered in the user MIME DB on first run, with
// a sniffer rule keyed on the header the writer emits ("DAW 1 "), and the
// resource file declares it as a supported type with B_SINGLE_LAUNCH: a second
// launch hands its files to the running instance instead of starting a second
// DAW.
//
// Usage:  daw [--version] [project.dawproj ...]
//
// Haiku-only: links the Interface Kit. The model layer it drives is kit-free.

#include "model/Project.h"
#include "model/Commands.h"
#include "model/PeakCache.h"
#include "plugin/PluginHost.h"
#include "ui/MainWindow.h"
#include "Version.h"   // DAW_VERSION_STRING, DAW_APP_SIGNATURE, DAW_PROJECT_MIME

// Defined by the daw_lv2 target, which exists only when CMake found lilv. With
// it absent nothing below is compiled, the LV2 factory hook is never installed,
// and an EffectType::Lv2 insert degrades to a null effect the same way an
// unavailable native add-on does.
#ifdef DAW_HAVE_LV2
#include "plugin/Lv2Host.h"
#endif

#include <Application.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <MimeType.h>
#include <Path.h>
#include <Roster.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace daw;

// The project/session a bare launch starts with: one audio track and one MIDI
// track, so the window is never blank and the synth is reachable.
static void SeedDemoTracks(Project& project, CommandStack& stack) {
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Audio, "Audio 1"), project);
    stack.Execute(std::make_unique<AddTrackCommand>(TrackType::Midi, "Synth"), project);
}

// Load native effect add-ons from a "plugins" dir next to the executable and
// from the user settings dir, so both a build-tree run and an installed run
// find them. Missing dirs are harmless.
static void InstallPlugins() {
    PluginHost& host = PluginHost::Instance();

    app_info info;
    if (be_app->GetAppInfo(&info) == B_OK) {
        BPath exe(&info.ref);
        BPath dir;
        if (exe.GetParent(&dir) == B_OK) {
            std::string p = std::string(dir.Path()) + "/plugins";
            host.ScanDir(p);
        }
    }

    BPath settings;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) == B_OK) {
        std::string p = std::string(settings.Path()) + "/HaikuDAW/plugins";
        host.ScanDir(p);
    }
}

// Scan the LV2 bundle path once at startup and install the EffectFactory hook.
// Reads the filesystem and parses RDF, so it belongs here beside InstallPlugins
// and nowhere near the audio thread. lilv uses its own default search path
// (LV2_PATH, else the system and per-user bundle directories), so there is no
// directory for us to name.
static void InstallLv2() {
#ifdef DAW_HAVE_LV2
    Lv2Host::Instance().ScanAll();
#endif
}

// Make a .dawproj open THIS app: install the type in the user MIME DB, describe
// it, point it at our signature, and give it a sniffer rule keyed on the
// writer's header line (ProjectIO writes "DAW 1 <flags>"). Idempotent and
// best-effort: a read-only or missing MIME DB must never stop the app from
// starting, and every later run re-asserts the association (a reinstall of the
// app or an edit in FileTypes can drop it).
static void RegisterProjectMimeType() {
    BMimeType type(DAW_PROJECT_MIME);
    if (!type.IsInstalled()) {
        if (type.Install() != B_OK) {
            std::fprintf(stderr, "daw: cannot register %s (read-only MIME DB?)\n",
                         DAW_PROJECT_MIME);
            return;
        }
        type.SetShortDescription("Haiku DAW project");
        type.SetLongDescription("A song project for Haiku DAW");
        type.SetSnifferRule("0.8 [0:6] ('DAW 1 ')");
    }
    type.SetPreferredApp(DAW_APP_SIGNATURE);
}

class DawApplication : public BApplication {
public:
    DawApplication() : BApplication(DAW_APP_SIGNATURE) {}

    void ReadyToRun() override {
        RegisterProjectMimeType();
        InstallPlugins();
        InstallLv2();
        std::fprintf(stderr, "Haiku DAW %s\n", DAW_VERSION_STRING);

        // A launch whose arguments name a project starts THERE, not in the
        // demo -- and nothing has to be discarded to get there (the pending
        // refs are opened below, after the window exists).
        if (fPending.empty())
            SeedDemoTracks(fProject, fStack);

        BRect frame(80, 80, 80 + 1000, 80 + 560);
        fWindow = new MainWindow(frame, &fProject, &fStack, &fPeaks);
        fWindow->Show();

        // Files that arrived before the window did (a first launch's argv or
        // refs race ReadyToRun): open them now.
        const std::vector<std::string> pending = fPending;
        fPending.clear();
        for (const std::string& path : pending)
            OpenPath(path.c_str());
    }

    // Files handed to an already-running instance: `open`, a double-click, a
    // drop on the Deskbar entry. The rdef's B_SINGLE_LAUNCH is what routes
    // them here instead of starting a second DAW.
    void RefsReceived(BMessage* message) override {
        entry_ref ref;
        for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
            BPath path(&ref);
            if (path.InitCheck() == B_OK)
                OpenPath(path.Path());
        }
    }

    // `daw project.dawproj` from a Terminal -- including the FIRST launch,
    // which receives its own argv here after Run(). Anything that is not a
    // project is ignored: the old "each argv WAV becomes a track" seed is
    // gone, because File > Import Audio is the way a file gets in.
    void ArgvReceived(int32 argc, char** argv) override {
        for (int32 i = 1; i < argc; i++)
            OpenPath(argv[i]);
    }

private:
    void OpenPath(const char* path) {
        if (!path || !path[0])
            return;
        const size_t len = std::strlen(path);
        if (len <= 8 || std::strcmp(path + len - 8, ".dawproj") != 0) {
            std::fprintf(stderr, "daw: ignoring '%s' (not a .dawproj)\n", path);
            return;
        }
        if (!fWindow) {                 // before ReadyToRun: open it there
            fPending.push_back(path);
            return;
        }
        BEntry entry(path);
        entry_ref ref;
        if (entry.GetRef(&ref) != B_OK) {
            std::fprintf(stderr, "daw: cannot open '%s'\n", path);
            return;
        }
        // One line, so a launch over SSH can be told from a no-op without a
        // window; the load itself reports only its failures.
        std::fprintf(stderr, "daw: opening %s\n", path);
        // Exactly what the open file panel posts: MSG_OPEN_REF (LoadFrom, with
        // its unsaved-changes prompt) is the single open path.
        BMessage open(MSG_OPEN_REF);
        open.AddRef("refs", &ref);
        fWindow->PostMessage(&open);
    }

    // The session: project, stack and peaks outlive the window (as they did
    // when they were main()'s statics).
    Project             fProject;
    CommandStack        fStack;
    MainWindow::PeakMap fPeaks;
    MainWindow*         fWindow = nullptr;
    std::vector<std::string> fPending;
};

int main(int argc, char** argv) {
    // --version answers without a window (and without an app_server, so a
    // package manager or a bug report can ask) -- before the application is
    // constructed, so a running instance cannot swallow it: the same string
    // the About box shows, from the one place CMake defines it.
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("Haiku DAW %s\n", DAW_VERSION_STRING);
            return 0;
        }
    }

    DawApplication app;
    app.Run();
    return 0;
}
