// SampleBrowser — a live BFS-query sample browser (the native-Haiku feature).
//
// Runs a BQuery over the boot volume for audio files matching a name filter,
// shows each with its DAW:duration / DAW:bpm attributes, and double-click
// imports the file into the timeline (posts kMsgBrowserImport to the main
// window). A BPM field tags the selected file's DAW:bpm attribute so a library
// can be built up. Haiku-only (Storage + Interface Kits).
#pragma once

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

class BListView;
class BTextControl;

namespace daw {

// Posted to the main window to import a browsed file. Fields: string "path";
// optional int64 "tid" + int64 "start" (drop target from a timeline drag-drop).
constexpr uint32 kMsgBrowserImport = 'bimp';
// The drag message a browser list row initiates; the timeline accepts it as a
// drop. Field: string "path".
constexpr uint32 kMsgSampleDrag = 'bsdg';

class SampleBrowser : public BWindow {
public:
    SampleBrowser(BRect frame, BMessenger target);

    void MessageReceived(BMessage* msg) override;

private:
    void RunQuery();          // (re)populate the list from the current filter
    std::string SelectedPath() const;

    BMessenger                fTarget;      // -> main window (import)
    BListView*                fList;
    BTextControl*             fFilter;      // name filter
    BTextControl*             fBpm;         // BPM to tag onto the selection
    std::vector<std::string>  fPaths;       // parallel to list items
};

} // namespace daw
