# Task: UX shell pass — the keyboard map, dialog buttons, Escape, feedback and vocabulary

You are working in a git worktree of `/home/marc/haiku-daw`, a native Haiku OS
DAW (C++17, Interface Kit). Branch `fix/ux-shell` off `master`. A UX review
produced a ranked findings list; **this file is that list and it is the scope** —
implement it, do not re-litigate it. Work items are A–E below.

This is a UI package: `docs/UI_GUIDELINES.md` is binding for everything under
`src/ui/`, and the orchestrator runs the `DAW_UI_SHOTS` pass afterwards — write
every change so a screenshot can show it, and name in the record what a reviewer
should look for.

## Codebase orientation (read before coding)

- `src/ui/MainWindow.cpp` — the main window: menu bar, transport wiring, the
  `DispatchMessage` key gate, the status line (`SetTransportStatus`,
  `UpdateTransportStatus`), the dock (`SetDockPage`).
- `src/model/KeyMap.h` — **the one keyboard map**: `FindKeyBinding()` resolves a
  byte + modifiers to a `KeyBinding` (and `BuildShortcutsText()` renders the same
  table into the help window). Kit-free, host-tested (`tests/keymap_tests.cpp`).
- `src/ui/widgets/` — the widget kit (M1.3): `DawButton`, `DawControl`, the
  control look. Haiku-only; verified on the VM.
- `src/ui/RenderJobs.cpp` — export + region render jobs. The region jobs set the
  status line when they finish (`:281-283, :318, :331, :357, :367, :391`); the
  export does not.
- The converted dialogs: `RenameWindow`, `QuantizeWindow`, `ExportWindow`,
  `NewProjectWindow`, `ProjectSettingsWindow`, `PreferencesWindow`.
- Haiku's default-button mechanism (verified against the Haiku sources under
  `~/haiku-cross/haiku`, which the syntax check already uses):
  `BWindow::SetDefaultButton(BButton*)` (`src/kits/interface/Window.cpp:1813`)
  stores the button and calls the **virtual** `BButton::MakeDefault(bool)` on it;
  `BWindow::_DetermineTarget` (`:3284`) routes a `B_ENTER` key-down to that
  button before the focus view, and `BButton::KeyDown` (`Button.cpp:242`)
  invokes on `B_ENTER`/`B_SPACE`. `B_CLOSE_ON_ESCAPE` is honoured in
  `BWindow::_HandleKeyDown` (`:3723`): Escape posts `B_QUIT_REQUESTED`.
- Host tests build with `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release &&
  cmake --build build-host -j2 && ctest --test-dir build-host`; `src/ui/` does
  not compile on Linux — cross-check pattern-faithfully with
  `sh scripts/haiku_syntax_check.sh` (four pre-existing FLAC/Vorbis FAILs).

## Goal

Five defects the review found, fixed without redesigning anything:

### A. The keyboard map dies when a non-text control has focus

`MainWindow::DispatchMessage` computes
`held = focus != nullptr && focus != fTimeline && !(isSpace && !textFocus)`, so
clicking the master volume `BSlider` (`MainWindow.cpp:538`) or the sample
browser's `BListView` swallows R/S/M/Home/`,`/`.`/arrows.

1. Gate on `textFocus` — a text field being typed into is the only thing that
   holds the map.
2. Resolve the key through the kit-free table first: a key `FindKeyBinding()`
   binds belongs to the app (the timeline), whatever control holds the focus;
   only an **unbound** key falls through to the focus view. Extract the rule as
   a kit-free `inline` function in `src/model/KeyMap.h` so the host suite can
   test it.
3. `ui_functional_tests`: focus the master slider, post `R`/`Home`/`m`, assert
   the effect (the suite already drives key posting — `TestKeyboardFocus`,
   `TestKeyMap`).

### B. No kit button is reachable by Tab, and Return never accepts a dialog

`DawButton`'s default flags lack `B_NAVIGABLE` while every other kit control
passes it; `DawButton::MakeDefault` only sets a private flag and draws a border —
nothing ever calls `BWindow::SetDefaultButton`, so Return does nothing in the
converted dialogs (Export, Project Settings, Rename, Preferences, New Project,
Quantize).

1. `B_NAVIGABLE` on the button's default flags.
2. `MakeDefault` routes to `BWindow::SetDefaultButton`. **The cast the API
   suggests is not safe**: `SetDefaultButton` calls the virtual
   `BButton::MakeDefault` on the stored pointer, so a pointer that is not
   actually a `BButton` walks the wrong vtable. Make `DawButton` derive from
   `BButton` (keeping the kit's own drawing and the `ThemeAware` hook) and the
   registration is type-safe with no cast.
3. Defer to attach: the dialogs call `MakeDefault(true)` while they build, before
   the button has a window; register in `AttachedToWindow` as well, and note that
   `MakeDefault(false)` must only clear the local flag — `BWindow` calls it from
   inside `SetDefaultButton`, where re-entering would recurse.
4. Make the primary button of each converted dialog the default (most already
   call `MakeDefault`; it has simply never worked).

### C. Escape cancels nothing outside alerts

No `B_CLOSE_ON_ESCAPE` anywhere in `src/ui`. Add it to the transient dialogs —
Rename, Quantize, Export options, New Project, Project Settings, Preferences —
and give Rename and Quantize an explicit **Cancel** button beside their primary
one, so every dialog has both a visible and a keyboard way out.

### D. Feedback the app never gives (`src/ui/RenderJobs.cpp` unless noted)

1. A finished export is silent: `FinishExport` reports success only to stderr
   (`:105-111`) while the region jobs set the status line. Set it on success
   ("Exported mix.wav"), and give the cancel and 0-byte/failed paths a line too.
2. A refused second export is silent: `StartExport` returns bare when a worker is
   live (`:29`) while `StartRegionJob` says "A render is already running"
   (`:148-150`). Mirror it.
3. Status messages never expire: make a job message clear after a few seconds.
   Timestamp it in `SetTransportStatus` and expire it from the existing 2 Hz
   `MSG_TITLE` poll (`MainWindow.cpp`) — no second runner.
4. The device-failure alerts point at a menu that no longer has the item
   ("Check the Audio menu's buffer size"); the buffer size moved to Preferences
   (M3.5). Correct both (`TransportController.cpp:74-79`, `:226-231`) to
   "File ▸ Preferences ▸ Buffer size".

### E. Vocabulary and titles — one pass, no redesign

- **Export options window title** names the action: "Export Mix" / "Export
  Stems", as its file panel already does.
- **Button vocabulary** — settled here, say it in the record:
  - *Settings-style windows* (the window holds values and leaving may discard):
    **Cancel + Apply**. Apply commits the whole view; Cancel closes and discards
    what was not applied; the old "Close" is removed from both.
    → Preferences (was "Close" + instant apply), Project Settings (was "Close"
    that discards + Apply).
  - *Action dialogs*: **Cancel + a verb**. New Project = Cancel/Create (already),
    Export = Cancel + Export Mix / Export Stems, Rename = Cancel + OK,
    Quantize = Cancel + Quantize.
- **"New"** opens a dialog but has no ellipsis while Save As…/Preferences… do.
- **The dock's Plugins page with no track selected** is inert and says "add to no
  track selected": answer a pick with a status line ("Select a track first") and
  reword the title ("Plugins — select a track to add to").
- **The Preferences device row** carries an internal roadmap note ("device
  selection arrives with the multichannel-input package"): reword for a user
  ("Default Output" with a note that choosing a device is not available yet).

## Rules

- Never change the OS's settings (no `set_ui_color`, no preferences writes).
- Two other agents edit `MainWindow.cpp` and `TimelineView.cpp` concurrently:
  keep `MainWindow.cpp` hunks tight and local, and put new MainWindow message
  ids in a separated block at the end of the MSG enum.
- Every behaviour change ships a test (host where the code is kit-free, else
  `ui_functional_tests` on the VM), mutation-checked — break it, watch the test
  fail, restore — and the mutations named in the record.
- Author `Marc Korte <marc@duskaudio.com>`, conventional commits, no
  AI-attribution trailer.
- Do not run `scripts/vm.sh` or the VM (the orchestrator owns the runs).

## Definition of done

- The key gate, `DawButton`'s flags and default registration, the status
  messages (success / refusal / expiry), the dialog vocabulary (Cancel + Apply,
  Cancel + verb), `B_CLOSE_ON_ESCAPE`, the window titles, the New… ellipsis, the
  Plugins page's no-track answer and the Preferences device copy are in.
- Host suite green (`ctest --test-dir build-host`, count pasted);
  `sh scripts/haiku_syntax_check.sh` with no NEW failures; the VM
  `ui_functional_tests` gains checks for A, B, C, D and the Preferences
  commit-on-Apply behaviour.
- `49-ux-shell-PR.md` records the vocabulary chosen, the tests and their
  mutations, what only the VM can prove, and what the screenshot pass should
  look at (a default button draws `B_DEFAULT_BUTTON`, a status line shows text,
  the Preferences window's two buttons, the Plugins page title with no track).
