// Host-buildable tests for the one keyboard map (daw::kKeyBindings).
//
// The point of the table (plan M3.2) is that the behaviour and the help read
// the same rows, so what is worth testing is the table's own consistency:
// nothing bound twice, every row usable and named, the plan's keys resolving
// to the right actions, and the generated help mentioning every row.
//
// Build/run:
//   g++ -std=c++17 -Isrc tests/keymap_tests.cpp -o /tmp/keys && /tmp/keys

#include "../src/model/KeyMap.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                       \
    do { ++g_checks; if (!(cond)) { ++g_fails;                            \
        std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); }   \
    } while (0)

using namespace daw;

// Which (char, mods) pairs a row answers to, for the duplicate rule.
static bool RowMatches(const KeyBinding& b, char key, uint32_t mods) {
    return FindKeyBindingExact(key, mods) == &b;
}

int main() {
    // --- every row is usable and named -----------------------------------
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        const KeyBinding& b = kKeyBindings[i];
        CHECK(b.key != 0);
        CHECK(b.keyAlt != b.key);
        CHECK(b.keyName != nullptr && b.keyName[0] != '\0');
        CHECK(b.help    != nullptr && b.help[0]    != '\0');
        CHECK(b.group   != nullptr && b.group[0]   != '\0');
        // A row the behaviour runs carries a real action; a documented menu
        // accelerator must not, or the window would have two owners.
        CHECK(b.route != KeyRoute::Dispatch || b.action != KeyAction::None);
        CHECK(b.route != KeyRoute::Menu || b.action == KeyAction::None);
        // Menu rows are all Command accelerators.
        CHECK(b.route != KeyRoute::Menu || (b.mods & kModCommand) != 0);
    }

    // Groups are contiguous: BuildShortcutsText prints a header whenever the
    // group changes, so a row out of order would show its section twice.
    {
        int runs = 0;
        std::vector<std::string> distinct;
        std::string cur;
        for (std::size_t i = 0; i < kKeyBindingCount; i++) {
            const std::string g = kKeyBindings[i].group;
            if (g != cur) { cur = g; runs++; }
            if (std::find(distinct.begin(), distinct.end(), g)
                == distinct.end())
                distinct.push_back(g);
        }
        CHECK(runs == (int)distinct.size());
    }

    // --- no key is bound twice -------------------------------------------
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        for (std::size_t j = i + 1; j < kKeyBindingCount; j++) {
            const KeyBinding& a = kKeyBindings[i];
            const KeyBinding& b = kKeyBindings[j];
            if (a.mods != b.mods) continue;
            const bool clash =
                (a.key == b.key) || (a.keyAlt != 0 && a.keyAlt == b.key)
                || (b.keyAlt != 0 && a.key == b.keyAlt)
                || (a.keyAlt != 0 && b.keyAlt != 0 && a.keyAlt == b.keyAlt)
                || (FoldKeyCase(a.key) == FoldKeyCase(b.key))
                || (a.keyAlt != 0
                    && FoldKeyCase(a.keyAlt) == FoldKeyCase(b.key))
                || (b.keyAlt != 0
                    && FoldKeyCase(a.key) == FoldKeyCase(b.keyAlt));
            CHECK(!clash);
            if (clash)
                std::printf("    '%s' and '%s' both claim '%c' + %u\n",
                            a.keyName, b.keyName, a.key, (unsigned)a.mods);
        }
    }

    // Every row is reachable through the lookup with its own key.
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        const KeyBinding& b = kKeyBindings[i];
        CHECK(RowMatches(b, b.key, b.mods));
        if (b.keyAlt != 0) CHECK(RowMatches(b, b.keyAlt, b.mods));
    }

    // --- the plan's keys --------------------------------------------------
    const KeyBinding* kb = nullptr;
    kb = FindKeyBinding(kKeyHome, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ReturnToZero);
    kb = FindKeyBinding(kKeyEnter, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ReturnToZero);
    kb = FindKeyBinding('r', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::RecordToggle);
    kb = FindKeyBinding('R', 0);            // Shift-R / Caps Lock
    CHECK(kb != nullptr && kb->action == KeyAction::RecordToggle);
    kb = FindKeyBinding('l', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::LoopToggle);
    kb = FindKeyBinding('l', kModShift);
    CHECK(kb != nullptr && kb->action == KeyAction::LoopBetweenMarkers);
    kb = FindKeyBinding('c', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ClickToggle);

    // --- what the window handled before still resolves --------------------
    kb = FindKeyBinding(kKeySpace, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::PlayStop);
    kb = FindKeyBinding(' ', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::PlayStop);
    kb = FindKeyBinding('m', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::DropMarker);
    kb = FindKeyBinding('M', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::DropMarker);
    kb = FindKeyBinding('f', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ZoomToFit);
    kb = FindKeyBinding('+', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ZoomIn);
    kb = FindKeyBinding('=', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ZoomIn);
    kb = FindKeyBinding('-', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ZoomOut);
    kb = FindKeyBinding('_', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ZoomOut);
    kb = FindKeyBinding(kKeyLeft, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::PanLeft);
    kb = FindKeyBinding(kKeyLeft, kModShift);
    CHECK(kb != nullptr && kb->action == KeyAction::PanLeft);
    kb = FindKeyBinding(kKeyLeft, kModCommand);
    CHECK(kb != nullptr && kb->action == KeyAction::MarkerPrev);
    kb = FindKeyBinding(kKeyRight, kModCommand);
    CHECK(kb != nullptr && kb->action == KeyAction::MarkerNext);
    kb = FindKeyBinding(kKeyDelete, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::DeleteSelection);
    kb = FindKeyBinding(kKeyBackspace, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::DeleteSelection);
    kb = FindKeyBinding(kKeyEscape, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ClearSelection);
    kb = FindKeyBinding(kKeyPageUp, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::PageUp);
    kb = FindKeyBinding(kKeyPageDown, 0);
    CHECK(kb != nullptr && kb->action == KeyAction::PageDown);
    // Ctrl-D arrives as 0x04 with the Control flag (see the table's comment).
    kb = FindKeyBinding(kKeyEnd, kModControl);
    CHECK(kb != nullptr && kb->action == KeyAction::DuplicateSelection);

    // --- the arrange keys M2 added, now rows of the same table -------------
    kb = FindKeyBinding('1', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ToolPointer);
    kb = FindKeyBinding('6', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::ToolFade);
    kb = FindKeyBinding('g', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::GridMenu);
    kb = FindKeyBinding('s', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::SplitAtPlayhead);
    kb = FindKeyBinding('s', kModCommand);   // Save stays the menu's
    CHECK(kb != nullptr && kb->route == KeyRoute::Menu);
    kb = FindKeyBinding(',', 0);
    CHECK(kb != nullptr && kb->action == KeyAction::NudgeLeft);
    kb = FindKeyBinding('<', kModShift);     // Shift-, arrives as '<'
    CHECK(kb != nullptr && kb->action == KeyAction::NudgeLeft);
    kb = FindKeyBinding('>', kModShift);
    CHECK(kb != nullptr && kb->action == KeyAction::NudgeRight);
    // Shift-M has a row of its own (the marker lane), so it no longer falls
    // through to plain M.
    kb = FindKeyBinding('M', kModShift);
    CHECK(kb != nullptr && kb->action == KeyAction::MarkerLaneToggle);

    // Lock keys are ignored, other modifiers are not.
    kb = FindKeyBinding('r', 0x08u /* B_CAPS_LOCK */ | 0x20u /* B_NUM_LOCK */);
    CHECK(kb != nullptr && kb->action == KeyAction::RecordToggle);
    // A combination nobody bound stays unbound -- the window's own menu
    // accelerators get their keys, not the timeline.
    CHECK(FindKeyBinding('r', kModCommand) == nullptr);
    CHECK(FindKeyBinding('x', 0) == nullptr);
    CHECK(FindKeyBinding(kKeyEnd, 0) == nullptr);   // plain End: nothing
    // Exact matching: L + Ctrl is not L.
    CHECK(FindKeyBinding('l', kModControl) == nullptr);

    // --- the routing gate (the UX-shell pass) -----------------------------
    // A text field being typed into owns the keyboard: nothing reaches the map.
    CHECK(!TimelineTakesKey('r', 0, true));
    CHECK(!TimelineTakesKey(kKeyHome, 0, true));
    CHECK(!TimelineTakesKey(' ', 0, true));

    // With any other view focused, a bound key is the map's -- the bug this
    // rule fixed: a focused slider or list used to swallow R/Home/M/,/. and
    // the arrows, so the transport was unreachable until the user clicked the
    // timeline back.
    CHECK(TimelineTakesKey('r', 0, false));
    CHECK(TimelineTakesKey('m', 0, false));
    CHECK(TimelineTakesKey(',', 0, false));
    CHECK(TimelineTakesKey('.', 0, false));
    CHECK(TimelineTakesKey(kKeyHome, 0, false));
    CHECK(TimelineTakesKey(kKeyLeft, 0, false));
    CHECK(TimelineTakesKey(kKeyRight, 0, false));
    CHECK(TimelineTakesKey(' ', 0, false));         // the transport's space
    CHECK(TimelineTakesKey('L', kModShift, false)); // case + modifiers agree

    // A key the table does not bind stays with the focused control (a list's
    // Up/Down, a view's own editing keys), so a control keeps what the map
    // has no opinion about.
    CHECK(!TimelineTakesKey(0x1e /* B_UP_ARROW */, 0, false));
    CHECK(!TimelineTakesKey(0x1f /* B_DOWN_ARROW */, 0, false));
    CHECK(!TimelineTakesKey('z', 0, false));
    CHECK(!TimelineTakesKey('r', kModCommand, false));   // Cmd-R: nobody's
    // A menu accelerator is the MENU's, never the timeline's.
    CHECK(!TimelineTakesKey('n', kModCommand, false));
    CHECK(!TimelineTakesKey('s', kModCommand, false));
    CHECK(!TimelineTakesKey('s', kModCommand | kModShift, false));

    // --- the help is generated from the table -----------------------------
    const std::string text = BuildShortcutsText();
    CHECK(!text.empty());
    for (std::size_t i = 0; i < kKeyBindingCount; i++) {
        const KeyBinding& b = kKeyBindings[i];
        CHECK(text.find(b.keyName) != std::string::npos);
        CHECK(text.find(b.help) != std::string::npos);
        // Exactly one entry per row: a row dropped from the text shows up as
        // a missing help line, and the group header is printed once.
        CHECK(text.find(b.group) != std::string::npos);
    }
    // Every row contributes exactly one line, plus a header for each group and
    // a blank line between groups -- so nothing can be silently dropped or
    // printed twice.
    std::size_t lines = 0, groups = 0;
    for (std::size_t i = 0; i < text.size(); i++)
        if (text[i] == '\n') lines++;
    for (std::size_t i = 0; i < kKeyBindingCount; i++)
        if (i == 0
            || std::strcmp(kKeyBindings[i].group, kKeyBindings[i - 1].group)
               != 0)
            groups++;
    CHECK(lines == kKeyBindingCount + groups + (groups > 0 ? groups - 1 : 0));

    std::printf("\n%d checks, %d failures (%d bindings)\n",
                g_checks, g_fails, (int)kKeyBindingCount);
    return g_fails == 0 ? 0 : 1;
}
