// ShortcutsWindow — the keyboard map, on screen (plan M3.2).
//
// The text is BuildShortcutsText() from the kit-free table (src/model/KeyMap.h)
// and nothing else, so the help cannot describe a binding the behaviour does
// not have. It opens from View > Keyboard Shortcuts; asking again brings the
// open window forward instead of stacking another (the mixer's pattern).
#pragma once

#include <Window.h>

#include <cstdint>

namespace daw {

// Sent to an already-open shortcuts window: come to the front.
constexpr uint32 kMsgShortcutsActivate = 'ksac';

// The view holding the text, by name, for the functional tests.
constexpr const char* kShortcutsTextView = "shortcuts";

class ShortcutsWindow : public BWindow {
public:
    // Sizes itself to the text (clamped to the screen) and shows.
    ShortcutsWindow();

    void MessageReceived(BMessage* msg) override;
};

} // namespace daw
