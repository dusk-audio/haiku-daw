# Task: Errors the user can see (plan M0.4)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/error-reporting` off master (M0.1–M0.3 and M0.6 are merged). This is
the last data-safety item of M0: today, load/save/record/device/plugin failures
print to stderr and nowhere else, so a user sees nothing.

## The state now

- `SaveTo`/`LoadFrom` failures: `std::fprintf(stderr, ...)` only (the prompt
  paths already refuse to proceed, but nothing says why).
- `Engine::Load` failure (three sites: play, record engine, loop-record) aborts
  with a stderr line.
- A running take: `Recorder`'s disk thread ignores `WavWriter`'s return values
  (`Recorder.cpp` write sites), and a failed open only prints.
- Load does not look for missing media at all; clips whose file is gone play
  silence and only the (external) tooling knows.
- A chain referencing a plugin that is not installed instantiates nothing; the
  insert row looks normal.

## Work items

1. **`MainWindow::ReportError(title, detail)`** — one helper, one look: a
   `BAlert` (`B_STOP_ALERT`) with the title and detail, opened asynchronously
   (`Go(nullptr)`, like the About box) so a report can never wedge the window
   thread. (M1 themes it; here it is the stock alert.) Route through it:
   - `LoadFrom` failure ("Open Project"), `SaveTo` failure ("Save Project");
   - `Engine::Load` failure at all three sites ("Audio Device": the device
     could not be opened; the project still plays nothing but nothing is lost);
   - `Recorder` open or write failure (below).
2. **Recorder failures reach the UI**: `Recorder` gains an atomic error code
   (`ErrorCode()`: 0 none, 1 open, 2 write/disk) set by the disk thread; the
   window's pulse sees a non-zero code while recording, reports ONCE, and stops
   the take — keeping what was written (stop already lands the partial take via
   `FramesWritten()`, which is the "disk full stops the take and keeps what was
   written" behaviour).
3. **Missing media on load**: after a successful load, collect every audio clip
   whose `sourcePath` does not exist; if any, ONE dialog lists them (first
   eight, "...and N more") with **Locate…** and **Skip**.
   - Skip leaves them as they are (silence, as today) — no further fuss.
   - Locate… walks the missing files one at a time (one open-mode file panel
     each; a cancelled panel ends the walk and keeps whatever was chosen so
     far), then applies the replacements as ONE `RelinkMediaCommand` (a macro,
     name "Locate Missing Media"), rebuilds peaks and invalidates the timeline.
   - `RelinkMediaCommand` is kit-free (`src/model/Commands.{h,cpp}`): a list of
     (track, clip, newPath); `Do` records the old paths, `Undo` puts them back.
     Host-tested (apply, undo, a vanished clip mid-way is a no-op not a
     failure).
4. **Missing plugin shows in the insert row**: `InspectorView` marks a row
   whose effect cannot resolve (built-in → always; add-on → the plugin host's
   registry; LV2 → `Lv2Host::Find`) as **"(missing)"** in its drawn label, so
   an uninstalled plugin is visible instead of silently silent. The lookup
   itself stays where the registries live.
5. **`ui_functional_tests`**: a save into an unwritable path raises the report
   (one window with the alert's title); a load of a project whose clip file was
   deleted raises the missing-media dialog with its two buttons; Skip dismisses
   it and leaves the clip; the recorder path cannot be driven (no capture) —
   say so.

## Definition of done

- Host suite green (`RelinkMediaCommand` tested; counts in the record);
  the new command mutation-checked (skip the undo restore; drop the capture of
  old paths).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL; VM `build` and `build-off` ctest
  green, `ui_functional_tests` included.
- PR record `docs/agent-prompts/14-errors-PR.md`; RELEASE_CHECKLIST item 27
  checked against what this actually delivers.
