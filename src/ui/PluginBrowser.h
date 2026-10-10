// PluginBrowser — one searchable list of everything that can go in an insert
// slot: built-in effects, native add-ons, and LV2 plugins.
//
// The effects editor previously grew one "Add ..." button per available effect,
// stacked below the chain. That does not survive a real plugin collection: the
// buttons sit below however many parameter rows the chain already draws, so with
// a 26-parameter plugin loaded they are pushed off the visible area entirely.
// A filtered list scales to any number of plugins.
//
// Two pieces since T2: `PluginBrowserView` is the content, and `PluginBrowser`
// is a window that hosts it. The dock hosts the same view, so there is one
// implementation of the list, the filter and the choice -- and the messages are
// unchanged either way.
//
// Follows the aux-window pattern: no model access, and the choice is posted on,
// because MainWindow (through the inspector) owns all mutation through the
// CommandStack. Haiku-only (Interface Kit).
#pragma once


#include "Theme.h"   // ThemeAware + the well colours (T1)
#include "../model/Effect.h"   // EffectType
#include "../model/types.h"    // TrackId

#include <GroupView.h>
#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

class BListView;
class BScrollView;
class BTextControl;

namespace daw {

// Posted to the effects editor when a plugin is chosen. Fields:
//   int64  "track"  — the chain being edited (kMasterFxTarget for master)
//   int32  "type"   — the EffectType to add
//   string "name"   — add-on id / LV2 URI; empty for built-ins
// The editor turns this into a descriptor (seeding host defaults) and applies
// the whole chain through the usual undoable path, so the browser itself never
// touches the model.
constexpr uint32 kMsgPluginChosen = 'pbch';

// A choice was made in a browser VIEW, to its own window. A windowed browser is
// a one-shot dialog -- it closes on the pick, as it always has -- while the
// docked page stays where the user put it, and the main window (which is the
// dock's window) has nothing to do about either.
constexpr uint32 kMsgBrowserDone = 'pbdn';

class PluginBrowserView : public BGroupView, public ThemeAware {
public:
    // `target` is where a choice is posted (the inspector, which turns it into
    // a SetFxCommand against "track"). `track` is the chain the choice applies
    // to; set it to kInvalidTrackId for "no track selected", which makes the
    // rows inert rather than inserting into the wrong track.
    PluginBrowserView(TrackId track, BMessenger target);

    // Retarget (the dock's page follows the selection): the same list, a
    // different chain.
    void SetTrack(TrackId track) { fTrack = track; }

    // The list is a stock BListView: it reads the system colour table unless
    // the theme's document colour is put on it (T1).
    void ApplyTheme() override;

    void MessageReceived(BMessage* msg) override;

private:
    // One offerable effect. Built-ins carry an empty `id`; plugins carry their
    // add-on name or LV2 URI, which is what EffectDesc.pluginName persists.
    struct Entry {
        EffectType  type;
        std::string id;
        std::string label;      // what the list shows, including its section
        std::string search;     // lowercased label, matched against the filter
    };

    void Rebuild();             // (re)populate the list from the current filter
    // `index` is the row the invocation named, or -1 to use the current
    // selection. The list can be rebuilt between a double-click and the message
    // arriving, which is why the invoked row is passed rather than re-read.
    void PostChoice(int32 index);

    BMessenger         fTarget;   // -> the inspector (the chosen plugin)
    TrackId            fTrack;
    BListView*         fList   = nullptr;
    BScrollView*       fScroll = nullptr;
    BTextControl*      fFilter = nullptr;
    std::vector<Entry> fAll;    // every entry, built once at construction
    std::vector<Entry> fShown;  // the filtered subset, parallel to the list rows
};

// The standalone window: the content view, plus the spacebar passthrough (a
// window that is not the main window has to forward Space to the transport
// itself).
class PluginBrowser : public BWindow {
public:
    // `main` is MainWindow, which is the only handler for the transport toggle.
    // It is a different window from `target`, so the spacebar passthrough cannot
    // reuse that one.
    PluginBrowser(BRect frame, TrackId track, BMessenger target,
                  BMessenger main);

    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport

private:
    BMessenger          fMain;
    PluginBrowserView*  fView = nullptr;
};

} // namespace daw
