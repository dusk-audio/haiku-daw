# feat: Tracker integration (plan M0.6)

Branch `feature/tracker-integration` off `master` (`51e52c5`). Commits: the
spec, the app + type registration, and the build fix that made any of it
reachable.

## What changed

- **`src/main.cpp`**: `DawApplication` replaces the plain `BApplication`.
  `ReadyToRun` owns startup (MIME registration, plugin + LV2 scans, the demo
  seed for a bare launch, the window); `RefsReceived` and `ArgvReceived` both
  funnel into `OpenPath`, which posts exactly what the File menu's open panel
  posts (`MSG_OPEN_REF`) so `LoadFrom` — with its unsaved-changes prompt —
  stays the single open path. A path arriving before the window exists is
  queued and flushed after `Show()`. Each open logs one stderr line. The stale
  WAV-argv seed (`SeedProject`, `BuildPeaks`, the `WavSource` include) is gone;
  a non-`.dawproj` argument is ignored with a note. `--version` still answers
  before the application exists, so a running instance cannot swallow it.
- **MIME type**: `CMakeLists` sets `DAW_PROJECT_MIME` once; `Version.h` and the
  rdef both read it. The app installs the type on first run (sniffer rule keyed
  on the writer's `DAW 1 ` header, this app as the preferred handler; a
  read-only DB must not stop a launch) and re-asserts the preferred app every
  run. The rdef gains `resource app_flags B_SINGLE_LAUNCH` and
  `resource file_types`.
- **`MSG_OPEN_REF`** moved to `MainWindow.h`'s public id block for the app to
  post (the identical move as on `feature/unsaved-changes`, so those merge).
- **Build**: the POST_BUILD step now runs `mimeset -A` after `xres`. This was
  the difference between "has resources" and "is an app": the roster reads a
  binary's **attributes**, not its resources, so without it a build-tree `daw`
  was a launch-flagless stranger and the roster could not route a file to a
  running instance at all. (Found by driving the flow on the VM: the binary had
  `BEOS:APP_SIG` as a *resource* and 0 bytes of attributes.)

## What is verified, and how (all over SSH on the VM, `build` dir)

- `./build/daw --version` → `Haiku DAW 1.0.0`, before any BApplication exists.
- `./build/daw /tmp/mini.dawproj` (argv at first launch) → stderr
  `daw: opening /tmp/mini.dawproj`, no load-failure line; one instance.
- With that instance running, a roster `Launch(&ref)` — the call Tracker's
  double-click and `open` both make — returns `Already running` and the running
  instance logs `daw: opening /boot/system/cache/tmp/mini.dawproj`; instance
  count stays 1 (the rdef's `B_SINGLE_LAUNCH` working end to end).
- With no instance, `Launch(&ref)` starts the app and it logs the same line
  (the launched app's stderr was captured by the probe).
- The type is installed at
  `/boot/home/config/settings/mime_db/application/x-dawproj` with
  `META:SNIFF_RULE` and `META:PREF_APP` set, and a header-only fixture is typed
  `application/x-dawproj` by sniffing.
- Suites: host 51/51; VM `build` 52/52 and `build-off` 48/48, both including
  `ui_functional_tests`; the POST_BUILD step (`rc` → `xres` → `mimeset -A`)
  runs green there, which is also rc's acceptance test for the new rdef.

## What is NOT verified

| Claim | State |
| --- | --- |
| `open project.dawproj` from a Terminal | `open` only relays the ref to Tracker (`src/bin/open.cpp`: `BMessenger(kTrackerSignature)`), and on this VM Tracker is behind the locked screen saver, so it did nothing. The call it makes (`be_roster->Launch`) is proven above. Confirm after unlocking the VM screen. |
| A real Tracker double-click | Same Launch call, proven; the double-click itself needs the screen unlocked (or Marc's box). |
| Refs arriving before `ReadyToRun` | The queue is there and compiles; the race is not forceable from a test. Both observed paths (argv at launch, refs to a running instance) went through `OpenPath` after the window existed. |
| Merge with `feature/unsaved-changes` | That branch adds `stack.MarkSaved()` right after the seed in `main.cpp`; here the seed moved into `DawApplication::ReadyToRun` (and is skipped when a project is handed in). The call belongs after `SeedDemoTracks` inside that branch. A small conflict; resolve by hand. |
