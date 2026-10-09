# Task: Tracker integration — open projects by double-click (plan M0.6)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/tracker-integration` off master. Plan item M0.6: the app is a native
Haiku citizen — a `.dawproj` in Tracker should open it, and the app should take
files handed to it (`open`, drag-onto-icon, `daw file.dawproj`).

## The state now

- `src/main.cpp` builds a plain `BApplication`, seeds a demo project, and treats
  every command-line argument as a **WAV to import** (`SeedProject` +
  `BuildPeaks`) — a leftover from before File ▸ Import existed. Nothing
  registers a project MIME type, so Tracker shows `.dawproj` as unknown, and
  the app has no `ArgvReceived`/`RefsReceived`.
- `MainWindow` already has the whole open path: `MSG_OPEN_REF` (a `BMessage`
  with an `entry_ref` under `"refs"`) → `LoadFrom` → the unsaved-changes prompt.
  An app-level hand-off must go through exactly that message — never straight
  into `LoadFrom`.
- The project file's first line is `DAW 1 1` (older files: `DAW 1`); that is
  what a sniffer rule can key on.
- `haiku-daw.rdef.in` carries the signature and version. `rc` + `xres` attach
  its resources to the `daw` binary (CMakeLists, `daw` target).

## Work items

1. **`DawApplication` (in `src/main.cpp`)**: a `BApplication` subclass that owns
   startup — plugin + LV2 scan, the demo seed for a bare launch, the window —
   and adds:
   - `RefsReceived`: every `entry_ref` under `"refs"` goes through the same
     `MSG_OPEN_REF` the file panel posts (so a dirty project is asked about);
   - `ArgvReceived`: `argv[1..]` paths ending in `.dawproj` do the same;
     anything else is ignored with a stderr note — the WAV-seeding path is gone;
   - a path arriving before the window exists (startup) is queued and flushed in
     `ReadyToRun`, after `Show()`.
   - one stderr line per opened path (`daw: opening <path>`), so a launch can be
     verified over SSH without eyes.
2. **MIME registration** (`ReadyToRun`, idempotent, best-effort): install
   `DAW_PROJECT_MIME`, set the short/long description, the preferred app
   (the signature), and the sniffer rule `0.8 [0:6] ('DAW 1 ')`. A read-only or
   missing MIME DB must not stop the app from starting.
3. **Single source for the type name**: `set(DAW_PROJECT_MIME ...)` in
   CMakeLists, used by `cmake/Version.h.in` (`DAW_PROJECT_MIME`) and by
   `haiku-daw.rdef.in` (`@DAW_PROJECT_MIME@`), exactly like the signature.
4. **The rdef**: `resource app_flags B_SINGLE_LAUNCH;` (one instance; a second
   launch hands its refs to the running one — which is what makes `open` work)
   and `resource file_types message { "types" = { "@DAW_PROJECT_MIME@" } };`.
5. **Delete the stale WAV-argv path**: `SeedProject`, `BuildPeaks` and the
   `WavSource` include go; `SeedDemoTracks` stays (a bare launch still starts
   with the demo project).

## Definition of done

- Host suite green (nothing kit-free changes; counts recorded).
- `sh scripts/haiku_syntax_check.sh src/main.cpp` 0 FAIL; VM `build` and
  `build-off` ctest green.
- On the VM, over SSH: `./build/daw /tmp/x.dawproj` logs `daw: opening ...` and
  does not log a load failure; with an instance already running,
  `open /tmp/x.dawproj` reaches that instance (single launch — no second
  process) and logs the same line; the user MIME DB gains the type. A screenshot
  proving the window shows the project is a bonus if the VM screen is unlocked.
- PR record `docs/agent-prompts/12-tracker-integration-PR.md`, including what
  a double-click in Tracker still needs a person to confirm.
