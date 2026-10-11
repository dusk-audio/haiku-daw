// MarkersWindow — the markers/locations list (M2.7).
//
// Like the mixer it runs on its OWN looper and holds NO model access: the main
// window pushes a snapshot of the marker list (kMsgMarkerList) whenever it
// changes, and every action the window offers travels back as an intent
// (kMsgMarkerJump / kMsgMarkerAdd / kMsgMarkerDelete, and RenameWindow's own
// kMsgRenameMarker for a rename). Nothing here may touch Project.
#pragma once

#include "Theme.h"   // ThemeAware + the tokens
#include "../model/types.h"

#include <Messenger.h>
#include <View.h>
#include <Window.h>

#include <string>
#include <vector>

namespace daw {

// One marker, as much of it as the list draws. `time` is pre-formatted by the
// main window (bar:beat + min:sec.mmm, the transport's readout) so this side
// needs neither the tempo map nor a sample rate.
struct MarkerInfo {
    Frame       frame = 0;
    std::string name;
    std::string time;
};

// MainWindow -> window: the whole list (int64 "frame", string "name", string
// "time" per marker) plus int64 "playhead".
constexpr uint32 kMsgMarkerList     = 'mkli';
// Bring the (already open) window to the front, like the mixer's activate.
constexpr uint32 kMsgMarkerActivate = 'mkac';
// window -> MainWindow intents.
constexpr uint32 kMsgMarkerJump     = 'mkjp';   // int64 "frame"
constexpr uint32 kMsgMarkerAdd      = 'mkad';   // add at the playhead
constexpr uint32 kMsgMarkerDelete   = 'mkdl';   // int64 "frame", string "name"

// The window's own buttons: they post these to their window, which runs the
// view's method (delete and rename need the selected row, which a fixed
// message cannot carry).
constexpr uint32 kMsgMarkerAddBtn    = 'mkab';
constexpr uint32 kMsgMarkerRenameBtn = 'mkrb';
constexpr uint32 kMsgMarkerDeleteBtn = 'mkdb';

class MarkersView : public BView, public ThemeAware {
public:
    // BView::Frame() would shadow the model's frame type for every unqualified
    // `Frame` here (docs/HANDOFF.md, the gotcha every view with frames hits).
    using Frame = daw::Frame;

    void ApplyTheme() override;   // T1: the cached panel colour

    MarkersView(BRect frame, std::vector<MarkerInfo> markers, Frame playhead,
                BMessenger apply);

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MessageReceived(BMessage* msg) override;   // wheel: scroll the list

    // Replace the list from a push (the main window owns the truth).
    void SetMarkers(std::vector<MarkerInfo> markers, Frame playhead);
    // The three actions the buttons run, as methods: the button posts here,
    // and so does a test that cannot press a button.
    void AddAtPlayhead();
    void RenameSelected();
    void DeleteSelected();
    int  SelectedRow() const { return fSelected; }
    const std::vector<MarkerInfo>& Markers() const { return fMarkers; }

private:
    float RowRectTop(int i) const;
    int   RowAt(BPoint where) const;
    void  Post(uint32 what);

    std::vector<MarkerInfo> fMarkers;
    Frame      fPlayhead = 0;
    int        fScroll   = 0;     // first visible row
    int        fSelected = -1;
    BMessenger fApply;            // the main window
};

class MarkersWindow : public BWindow {
public:
    using Frame = daw::Frame;

    MarkersWindow(BRect frame, std::vector<MarkerInfo> markers, Frame playhead,
                  BMessenger apply);
    void MessageReceived(BMessage* msg) override;
    // Space plays/stops and Cmd-Z undoes from here, like every child window.
    void DispatchMessage(BMessage* msg, BHandler* handler) override;

    MarkersView* View() const { return fView; }

private:
    MarkersView* fView = nullptr;
    BMessenger   fApply;
};

} // namespace daw
