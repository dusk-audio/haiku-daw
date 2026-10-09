# Task: The File menu — New, Open Recent, Save/Save As, Close (plan M0.2)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/file-menu` off master. Plan item M0.2, on top of the merged M0.1 (dirty
tracking, `ConfirmDiscardChanges`) and M0.6 (`MSG_OPEN_REF` is public).

## The state now

`MainWindow`'s File menu is Open / Save (always the panel) / Import ×2 /
Export ×3 / Quit. There is no New, no Save As, no recent list, and Save asks
for a path every time even after the project has one. `AppSettings`
(`src/app/AppSettings.{h,cpp}`, kit-free, host-tested) is the settings file
(`key value` lines; `lastdir` takes the rest of its line, for spaces).

## Work items

1. **`AppSettings` gains the recent list**: `std::vector<std::string>
   recentProjects` (at most `kMaxRecent` = 10), one `recent <path>` line each
   (rest-of-line, like `lastdir`), plus a kit-free `RememberRecent(path)` that
   moves an existing entry to the front, drops duplicates, and truncates to 10.
   Host-test: round trip, order, dedupe, cap, and a path with spaces.
2. **The menu** (File): New (Cmd-N), Open (Cmd-O, existing), Open Recent ▸
   (submenu, rebuilt whenever the list changes), Save (Cmd-S), Save As…
   (Shift-Cmd-S), separator, Close (Cmd-W), Quit (Cmd-Q). Close posts
   `B_QUIT_REQUESTED` to the window -- the same path as the close button, so
   the unsaved-changes prompt applies.
3. **Save is silent once the project has a path** (`MSG_SAVE` →
   `SaveTo(fProjectPath)`); Save As always opens the panel (shares the
   existing one). Both record the path with `RememberRecent` and persist it.
4. **New** (`MainWindow::NewProject`, Cmd-N): ask first
   (`ConfirmDiscardChanges`), stop the transport, flush + close the editors,
   reset the model to an empty `Project` **keeping the session's sample rate**,
   clear the stack and `MarkSaved()` (a new project is clean), forget the path
   and take directory, refresh the title, the peaks, the master fader, the
   timeline and the time readout. Same "stopped" precedent as `LoadFrom` about
   the engine (it rebuilds at the next play).
5. **Open Recent entries**: an entry whose file no longer exists is pruned from
   the list (with a stderr note) instead of being handed to `LoadFrom`; a live
   one goes through `LoadFrom` like any other open (prompt included).
6. **`ui_functional_tests`**: the plan's "Save As then Save" flow — Save As
   answers the panel and writes the file; Save afterwards writes the same path
   with NO panel and clears the dirty marker; and a New over a dirty project
   prompts, and Discard leaves an empty project, an `Untitled` title and no
   marker.

## Definition of done

- Host suite green (the settings tests grow; counts in the record); the new
  settings tests mutation-checked (drop the dedupe; drop the cap).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and `build-off` ctest
  green, `ui_functional_tests` included.
- PR record `docs/agent-prompts/13-file-menu-PR.md`.
