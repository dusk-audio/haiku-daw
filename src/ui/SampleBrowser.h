// SampleBrowser — a live BFS-query sample browser (the native-Haiku feature).
//
// Runs a BQuery over the boot volume for audio files matching a name filter,
// shows each with its DAW:duration / DAW:bpm attributes, and double-click
// imports the file into the timeline (posts kMsgBrowserImport to the main
// window). A BPM field tags the selected file's DAW:bpm attribute so a library
// can be built up.
//
// Two pieces since T2: `SampleBrowserView` is the content and `SampleBrowser`
// is a window hosting it, so the dock can show the same view. The window used
// absolute rectangles measured from its own bounds; the view is built with the
// Layout Kit, which is what lets it live in a pane of any size.
// Haiku-only (Storage + Interface Kits).
#pragma once


#include "Theme.h"   // ThemeAware + the well colours (T1)

#include <GroupView.h>
#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

class BListView;
class BScrollView;
class BTextControl;

namespace daw {

// Posted to the main window to import a browsed file. Fields: string "path";
// optional int64 "tid" + int64 "start" (drop target from a timeline drag-drop).
constexpr uint32 kMsgBrowserImport = 'bimp';
// The drag message a browser list row initiates; the timeline accepts it as a
// drop. Field: string "path".
constexpr uint32 kMsgSampleDrag = 'bsdg';

class SampleBrowserView : public BGroupView, public ThemeAware {
public:
    explicit SampleBrowserView(BMessenger target);

    // The list is a stock BListView subclass: without this it keeps the
    // system's list colour in Dark mode (T1).
    void ApplyTheme() override;

    void AttachedToWindow() override;

    void MessageReceived(BMessage* msg) override;

private:
    void RunQuery();          // (re)populate the list from the current filter
    std::string SelectedPath() const;

    BMessenger                fTarget;      // -> main window (import)
    BListView*                fList = nullptr;
    BScrollView*              fScroll = nullptr;
    BTextControl*             fFilter = nullptr;   // name filter
    BTextControl*             fBpm = nullptr;      // BPM to tag onto the selection
    std::vector<std::string>  fPaths;       // parallel to list items
};

// The standalone window (File ▸ Sample Browser, View ▸ Sample Browser).
class SampleBrowser : public BWindow {
public:
    SampleBrowser(BRect frame, BMessenger target);

private:
    SampleBrowserView* fView = nullptr;
};

} // namespace daw
