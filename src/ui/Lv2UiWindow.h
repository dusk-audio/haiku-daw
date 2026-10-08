// Lv2UiWindow — open an LV2 plugin's OWN GUI in a window of ours.
//
// The DAW hosts LV2 DSP and draws a generic parameter list for it. This is the
// other half: the editor a plugin ships, embedded in a Haiku window.
//
// TWO MODES, decided per plugin by what its UI asks the host for:
//
//  - A CONTROL-PORT UI writes through the LV2UI_Write_Function. Those writes go
//    straight out as kMsgFxLive, the same single live channel the generic
//    parameter panel uses, and are committed to the model on a debounce. The
//    editor is then live: a knob move is audible as it happens.
//
//  - A DIRECT_ACCESS UI does not use that function at all. It is handed the DSP
//    instance through instance-access / data-access and pokes it directly, so
//    linking it means handing a GUI thread a pointer the audio thread runs
//    every block, with no lock the plugin knows to take. That crosses the
//    real-time boundary deliberately and is NOT done here: such a UI keeps its
//    OWN instance, seeded with the insert's stored values, and changes nothing.
//
// The window title says which mode it is in, because the difference is audible
// and a plugin editor that silently did nothing would be worse than one that
// says so.
//
// Haiku-only (Interface Kit), and needs lilv.
#pragma once

#include "model/types.h"

#include <Messenger.h>
#include <Window.h>

#include <string>
#include <vector>

namespace daw {

class Lv2UiWindow : public BWindow {
public:
    // True if `pluginUri` ships a UI this host can embed (Haiku's native type).
    // Cheap enough to call while drawing a panel: it parses the world once and
    // caches the answer.
    static bool HasNativeUi(const std::string& pluginUri);

    // Opens the plugin's editor for the insert at `fxIndex` of `track`, seeded
    // with `params` (that insert's stored values, slot order).
    //
    // `track` is the chain's address as the rest of the UI writes it, master
    // sentinel included, and `apply` is the MainWindow messenger: a live editor
    // addresses its insert ONLY by (track, fxIndex, slot) through those, never
    // by a cached pointer, because the engine rebuilds its chain on structural
    // edits and on play and a cached instance would dangle across that.
    //
    // Returns nullptr when the plugin has no embeddable UI, when its editor
    // refuses to instantiate, OR when an editor for this plugin is already open
    // (that one is raised instead). A null return means "nothing new appeared",
    // not "this failed" -- callers that want the generic panel as a fallback
    // must ask HasNativeUi() first rather than treating null as absence.
    static Lv2UiWindow* Open(BRect frame, const std::string& pluginUri,
                             const std::string& displayName,
                             const std::vector<float>& params,
                             TrackId track, int fxIndex, BMessenger apply);

    ~Lv2UiWindow() override;
    bool QuitRequested() override;
    void MessageReceived(BMessage* msg) override;   // "raise yourself"

private:
    Lv2UiWindow(BRect frame, const char* title);

    // Post the values a live editor has written but not yet committed, as one
    // undoable edit. Runs on the debounce timer and once more on the way out,
    // so closing a window mid-gesture cannot leave the model behind the audio.
    void CommitPending();

    struct Impl;
    Impl* fImpl = nullptr;
};

} // namespace daw
