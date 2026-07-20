// Lv2UiWindow — open an LV2 plugin's OWN GUI in a window of ours.
//
// The DAW hosts LV2 DSP and draws a generic parameter list for it. This is the
// other half: the editor a plugin ships, embedded in a Haiku window.
//
// READ-ONLY FOR NOW, and the reason is worth stating rather than discovering.
// A plugin UI talks to a DSP instance through instance-access, and the instance
// the DAW is actually playing lives inside the engine's chain and is touched by
// the audio thread every block. Handing a GUI a pointer into that is normal for
// an LV2 host, but it crosses the real-time boundary the engine has been careful
// about, so it is a deliberate decision and not a side effect of adding a
// window. Until that is decided, this opens its OWN instance of the plugin,
// seeded with the insert's stored parameter values.
//
// So: the editor is the real one, it displays the insert's real settings, and
// moving its controls does NOT change what you hear. That limitation is shown in
// the window title, because a plugin editor that silently did nothing would be
// worse than one that says so.
//
// Haiku-only (Interface Kit), and needs lilv.
#pragma once

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

    // Opens the plugin's editor, seeded with `params` (the insert's stored
    // values, slot order). Returns nullptr when the plugin has no embeddable
    // UI or its editor refuses to instantiate — the caller then keeps using the
    // generic panel.
    static Lv2UiWindow* Open(BRect frame, const std::string& pluginUri,
                             const std::string& displayName,
                             const std::vector<float>& params);

    ~Lv2UiWindow() override;
    bool QuitRequested() override;

private:
    Lv2UiWindow(BRect frame, const char* title);

    struct Impl;
    Impl* fImpl = nullptr;
};

} // namespace daw
