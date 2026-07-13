// MainWindow — top-level DAW window.
//
// Holds the timeline view (M4b). Transport bar, track-header controls, and
// the menu/undo wiring arrive in later M4 sub-milestones. Owns nothing about
// audio yet; it renders the Project handed to it.
#pragma once

#include "../model/Project.h"
#include "../model/PeakCache.h"

#include <Window.h>

#include <map>
#include <string>

namespace daw {

class TimelineView;

class MainWindow : public BWindow {
public:
    using PeakMap = std::map<std::string, PeakCache>;
    MainWindow(BRect frame, const Project* project, const PeakMap* peaks);

    bool QuitRequested() override;   // quit the app when the window closes

private:
    TimelineView* fTimeline;
};

} // namespace daw
