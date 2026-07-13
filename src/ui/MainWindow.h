// MainWindow — top-level DAW window.
//
// Layout: a transport bar (play/stop + time readout) across the top, the
// timeline view filling the rest. Owns the playback Engine: Play builds it
// from the current Project and starts it (rebuild-on-play — RT-safe, matches
// how the engine already loads a session); a BMessageRunner polls the
// engine's atomic playhead ~60 Hz and drives the timeline's playhead line.
//
// Track-header controls + command/undo wiring arrive in M4e.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"
#include "../model/Command.h"
#include "../engine/Engine.h"

#include <Window.h>

#include <map>
#include <memory>
#include <string>

class BButton;
class BStringView;
class BMessageRunner;

namespace daw {

class TimelineView;

class MainWindow : public BWindow {
public:
    // Like TimelineView: BWindow::Frame() would shadow the model's frame type
    // for every unqualified `Frame` in this class. A member typedef hides it.
    using Frame = daw::Frame;

    using PeakMap = std::map<std::string, PeakCache>;
    MainWindow(BRect frame, Project* project, CommandStack* stack,
               const PeakMap* peaks);
    ~MainWindow() override;

    void MessageReceived(BMessage* msg) override;
    bool QuitRequested() override;   // quit the app when the window closes

private:
    void StartPlayback();
    void StopPlayback();
    void UpdateTimeReadout(Frame playhead);

    Project*        fProject;        // non-owning (the session)
    CommandStack*   fStack;          // non-owning

    TimelineView*   fTimeline;
    BStringView*    fTimeView;

    std::unique_ptr<Engine> fEngine;         // rebuilt each Play
    BMessageRunner*         fPulse = nullptr; // playhead poll, deleted on stop
};

} // namespace daw
