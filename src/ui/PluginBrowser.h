// PluginBrowser — one searchable list of everything that can go in an insert
// slot: built-in effects, native add-ons, and LV2 plugins.
//
// The effects editor previously grew one "Add ..." button per available effect,
// stacked below the chain. That does not survive a real plugin collection: the
// buttons sit below however many parameter rows the chain already draws, so with
// a 26-parameter plugin loaded they are pushed off the visible area entirely.
// A filtered list scales to any number of plugins and is the same shape the
// sample browser already uses (BWindow + BListView + a filter field).
//
// Follows the aux-window pattern: own looper, no model access, and the choice is
// posted to MainWindow, which owns all mutation through the CommandStack.
// Haiku-only (Interface Kit).
#pragma once


#include "Theme.h"   // ThemeAware + the well colours (T1)
#include "../model/Effect.h"   // EffectType
#include "../model/types.h"    // TrackId

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

class PluginBrowser : public BWindow, public ThemeAware {
public:
    // The list is a stock BListView: it reads the system colour table unless
    // the theme's document colour is put on it (T1).
    void ApplyTheme() override;

public:
    // `target` is the effects editor the choice is posted back to; `main` is
    // MainWindow, which is the only handler for the transport toggle. They are
    // different windows, so the spacebar passthrough cannot reuse `target`.
    PluginBrowser(BRect frame, TrackId track, BMessenger target,
                  BMessenger main);

    void MessageReceived(BMessage* msg) override;
    void DispatchMessage(BMessage* msg, BHandler* h) override;  // spacebar -> transport

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

    BMessenger         fTarget;   // -> effects editor (the chosen plugin)
    BMessenger         fMain;     // -> MainWindow (spacebar transport only)
    TrackId            fTrack;
    BListView*         fList   = nullptr;
    BScrollView*       fScroll = nullptr;
    BTextControl*      fFilter = nullptr;
    std::vector<Entry> fAll;    // every entry, built once at construction
    std::vector<Entry> fShown;  // the filtered subset, parallel to the list rows
};

} // namespace daw
