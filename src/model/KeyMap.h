// KeyMap — the one keyboard map (plan M3.2).
//
// Every binding the arrange window and the app's transport have is a row in
// kKeyBindings below. Two things read that table and nothing else:
//
//   * the behaviour — TimelineView::KeyDown resolves the pressed byte through
//     FindKeyBinding() and switches on the ACTION, so no key literal is
//     compared anywhere in the UI;
//   * the help — BuildShortcutsText() renders the same rows into the text the
//     Keyboard Shortcuts window shows.
//
// That is the point of the item: a binding cannot exist in the behaviour and
// be missing from the help (or the reverse), because there is only one copy.
//
// Kit-free (STL only) and header-only, like the other shared helpers in
// src/model/ — the host tests and the UI read the same bytes, and
// tests/keymap_tests.cpp checks the table is internally consistent. The key
// codes are the plain bytes a BView's KeyDown receives; the Interface Kit's
// B_HOME and friends are the same values, and src/ui/TimelineView.cpp pins
// each special key with a static_assert so the two cannot drift.
//
// What is deliberately NOT in this table: the piano roll's editor-local keys
// (1-7 pick a tool, q quantizes, Cmd-A selects all). They belong to that
// editor's own window, which shows its toolbar; the arrange window's map is
// what this table is.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace daw {

// --- key codes ------------------------------------------------------------
// The byte KeyDown receives for the non-printing keys (BeOS codes; see the
// static_asserts in src/ui/TimelineView.cpp).
constexpr char kKeyHome     = 0x01;
constexpr char kKeyEnd      = 0x04;   // also the byte Ctrl-D arrives as
constexpr char kKeyBackspace = 0x08;
constexpr char kKeyEnter    = 0x0a;   // == B_RETURN
constexpr char kKeyPageUp   = 0x0b;
constexpr char kKeyPageDown = 0x0c;
constexpr char kKeyEscape   = 0x1b;
constexpr char kKeyLeft     = 0x1c;
constexpr char kKeyRight    = 0x1d;
constexpr char kKeySpace    = 0x20;
constexpr char kKeyDelete   = 0x7f;

// --- modifiers ------------------------------------------------------------
// The Interface Kit's B_*_KEY masks (pinned by static_assert in the UI).
// kModMask is what a binding is matched against: the lock keys are dropped, so
// Caps Lock does not stop 'r' from recording.
constexpr uint32_t kModShift   = 0x00000001;
constexpr uint32_t kModCommand = 0x00000002;
constexpr uint32_t kModControl = 0x00000004;
constexpr uint32_t kModOption  = 0x00000040;
constexpr uint32_t kModMask    = kModShift | kModCommand | kModControl
                                 | kModOption;

// --- the actions ----------------------------------------------------------

// One id per thing a key makes the app do. TimelineView::KeyDown switches on
// this, never on the key.
enum class KeyAction {
    None,               // documented only (a menu accelerator; see KeyRoute)
    // Transport
    PlayStop,
    RecordToggle,
    LoopToggle,
    LoopBetweenMarkers,
    ClickToggle,
    ReturnToZero,
    // View / navigation
    PanLeft,
    PanRight,
    MarkerPrev,
    MarkerNext,
    ZoomIn,
    ZoomOut,
    ZoomToFit,
    PageUp,
    PageDown,
    // Editing
    DeleteSelection,
    DuplicateSelection,
    ClearSelection,
    DropMarker,
};

// Who runs the binding. `Menu` rows are accelerators a BMenuItem owns (the
// menu dispatches them before any view sees the key) — the table documents
// them so the help is complete, and the behaviour must NOT handle them.
enum class KeyRoute { Dispatch, Menu };

struct KeyBinding {
    char        key;      // the byte KeyDown receives
    char        keyAlt;   // a second spelling of the same key (0 = none)
    uint32_t    mods;     // required modifiers; matched exactly after masking
    KeyAction   action;
    KeyRoute    route;
    const char* keyName;  // how the help prints the key
    const char* help;     // one line: what it does
    const char* group;    // the help section it belongs to
};

// The map, in help order. Groups are contiguous (BuildShortcutsText relies on
// it, and the host test checks it).
inline constexpr KeyBinding kKeyBindings[] = {
    // --- transport ---
    { kKeySpace, 0, 0, KeyAction::PlayStop, KeyRoute::Dispatch,
      "Space", "Play / stop", "Transport" },
    { 'r', 0, 0, KeyAction::RecordToggle, KeyRoute::Dispatch,
      "R", "Record", "Transport" },
    { 'l', 0, 0, KeyAction::LoopToggle, KeyRoute::Dispatch,
      "L", "Loop on / off (uses the ruler's loop range)", "Transport" },
    { 'l', 0, kModShift, KeyAction::LoopBetweenMarkers, KeyRoute::Dispatch,
      "Shift-L", "Loop between the markers around the playhead", "Transport" },
    { 'c', 0, 0, KeyAction::ClickToggle, KeyRoute::Dispatch,
      "C", "Click (metronome) on / off", "Transport" },
    { kKeyHome, 0, 0, KeyAction::ReturnToZero, KeyRoute::Dispatch,
      "Home", "Go to the start", "Transport" },
    { kKeyEnter, 0, 0, KeyAction::ReturnToZero, KeyRoute::Dispatch,
      "Enter", "Return to zero", "Transport" },

    // --- view ---
    { kKeyLeft, 0, 0, KeyAction::PanLeft, KeyRoute::Dispatch,
      "Left", "Pan the view left", "View" },
    { kKeyRight, 0, 0, KeyAction::PanRight, KeyRoute::Dispatch,
      "Right", "Pan the view right", "View" },
    { kKeyLeft, 0, kModCommand, KeyAction::MarkerPrev, KeyRoute::Dispatch,
      "Cmd-Left", "Jump to the previous marker", "View" },
    { kKeyRight, 0, kModCommand, KeyAction::MarkerNext, KeyRoute::Dispatch,
      "Cmd-Right", "Jump to the next marker", "View" },
    { '+', '=', 0, KeyAction::ZoomIn, KeyRoute::Dispatch,
      "+", "Zoom in", "View" },
    { '-', '_', 0, KeyAction::ZoomOut, KeyRoute::Dispatch,
      "-", "Zoom out", "View" },
    { 'f', 0, 0, KeyAction::ZoomToFit, KeyRoute::Dispatch,
      "F", "Zoom to fit", "View" },
    { kKeyPageUp, 0, 0, KeyAction::PageUp, KeyRoute::Dispatch,
      "Page Up", "Scroll up a page", "View" },
    { kKeyPageDown, 0, 0, KeyAction::PageDown, KeyRoute::Dispatch,
      "Page Down", "Scroll down a page", "View" },

    // --- editing ---
    { kKeyDelete, kKeyBackspace, 0, KeyAction::DeleteSelection,
      KeyRoute::Dispatch, "Del", "Delete the selection", "Editing" },
    // Ctrl-D arrives as the control character 0x04 (the keyboard's control
    // map), not as 'd' with the Control flag -- the old `case 'd'` never
    // fired, so Duplicate had no working key.
    { kKeyEnd, 0, kModControl, KeyAction::DuplicateSelection,
      KeyRoute::Dispatch, "Ctrl-D", "Duplicate the selection", "Editing" },
    { kKeyEscape, 0, 0, KeyAction::ClearSelection, KeyRoute::Dispatch,
      "Esc", "Clear the selection", "Editing" },
    { 'm', 0, 0, KeyAction::DropMarker, KeyRoute::Dispatch,
      "M", "Drop a marker at the playhead", "Editing" },

    // --- the menu bar's accelerators (documented, dispatched by BMenuItem) ---
    { 'n', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-N", "New project (File)", "Menus" },
    { 'o', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-O", "Open project (File)", "Menus" },
    { 's', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-S", "Save (File)", "Menus" },
    { 's', 0, kModCommand | kModShift, KeyAction::None, KeyRoute::Menu,
      "Cmd-Shift-S", "Save As (File)", "Menus" },
    { 'w', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-W", "Close the window (File)", "Menus" },
    { 'q', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-Q", "Quit (File)", "Menus" },
    { 'z', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-Z", "Undo (Edit)", "Menus" },
    { 'z', 0, kModCommand | kModShift, KeyAction::None, KeyRoute::Menu,
      "Cmd-Shift-Z", "Redo (Edit)", "Menus" },
    { 'v', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-V", "Paste (Edit)", "Menus" },
    { 'i', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-I", "Show / hide the inspector (View)", "Menus" },
    { 'j', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-J", "Show / hide the editor & browsers (View)", "Menus" },
    { 'f', 0, kModCommand, KeyAction::None, KeyRoute::Menu,
      "Cmd-F", "Zoom to fit (View)", "Menus" },
};

inline constexpr std::size_t kKeyBindingCount =
    sizeof(kKeyBindings) / sizeof(kKeyBindings[0]);

// Letters are matched case-insensitively (KeyDown delivers 'R' for Shift-R):
// what makes a binding distinct is the modifier set, not the shift state of
// the byte. Non-letters are untouched.
inline char FoldKeyCase(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Modifiers are matched exactly over kModMask, so Ctrl-D does not also fire
// on Cmd-Ctrl-D, and plain L is not the Shift-L row.
inline const KeyBinding* FindKeyBindingExact(char key, uint32_t mods) {
    const char folded = FoldKeyCase(key);
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        const KeyBinding& b = kKeyBindings[i];
        const bool hit = (b.key == key)
                      || (b.keyAlt != 0 && b.keyAlt == key)
                      || FoldKeyCase(b.key) == folded;
        if (hit && b.mods == mods) return &b;
    }
    return nullptr;
}

// The binding for this key press, or nullptr when nothing is bound to it.
inline const KeyBinding* FindKeyBinding(char key, uint32_t modifiers) {
    const uint32_t m = modifiers & kModMask;
    if (const KeyBinding* b = FindKeyBindingExact(key, m)) return b;
    // A key with no Shift binding of its own keeps the unshifted one: Shift-M
    // still drops a marker and Shift-Left still pans, while Shift-L finds its
    // own row in the exact pass and never falls through to plain L.
    if (m & kModShift)
        return FindKeyBindingExact(key, m & ~kModShift);
    return nullptr;
}

// The help text, generated from the table: the group's name, then one
// "<key name>\t<what it does>" line per row. The window sets a tab width, so
// the descriptions line up.
inline std::string BuildShortcutsText() {
    std::string out;
    const char* group = nullptr;
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        const KeyBinding& b = kKeyBindings[i];
        if (group == nullptr || std::strcmp(group, b.group) != 0) {
            if (!out.empty()) out += "\n";
            out += b.group;
            out += "\n";
            group = b.group;
        }
        out += "    ";
        out += b.keyName;
        out += "\t";
        out += b.help;
        out += "\n";
    }
    return out;
}

} // namespace daw
