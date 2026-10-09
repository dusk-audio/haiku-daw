# feat: errors the user can see (plan M0.4)

Branch `feature/error-reporting` off `master` (`f8c76f2`, M0.1–M0.3 and M0.6
merged). The last data-safety item of M0: failures reached stderr and nowhere
else.

## What changed

- **`MainWindow::ReportError(title, detail)`** — one helper, one look: a
  `B_STOP_ALERT` opened asynchronously (`Go(nullptr)`), so a report can never
  hold the window thread. M1 gives it the themed look.
- **Routed through it**: `SaveTo` and `LoadFrom` failures (with "the session is
  untouched" in the detail), `Engine::Load` failures at all four sites, and the
  Recorder's.
- **`Engine::Load` distinguishes the two failures** it always conflated:
  `B_ENTRY_NOT_FOUND` now means "no content and the caller did not extend the
  range" (Play on an empty project is a fact, not an alert) while everything
  else is the output device. Documented on the declaration.
- **`Recorder`** gains an atomic error code (1 = the take file could not be
  created, 2 = a write failed); the disk thread's `WriteInt16` results are
  finally checked. The pulse reports ONCE and stops the take — `FramesWritten`
  lands what was written, so a full disk costs the take's tail, not the take.
- **Missing media after a load**: `CollectMissingMedia` gathers audio clips
  whose file is gone and raises ONE dialog listing them (first eight, "...and
  more") with **Skip** and **Locate…**. Skip leaves the clips silent, exactly
  as they have always been. Locate… walks the files one panel at a time (the
  panel starts in the directory the file used to live in; a cancelled panel
  posts nothing and ends the walk, keeping what was already chosen) and applies
  the whole repair as one **`RelinkMediaCommand`** (`src/model/Commands.*`,
  kit-free): `Do` records each clip's old path as it changes it, `Undo`
  restores exactly those, and an entry whose clip has vanished is skipped
  rather than failing the repair.
- **`InspectorView`** appends "  (missing)" to an insert row whose effect
  cannot resolve: built-ins always; add-ons by `PluginHost`'s registry; LV2 by
  `Lv2Host::Find` (and never in a build without LV2). A chain naming an
  uninstalled plugin finally says why it is silent.

## What is verified, and how

- **Host: 52/52**; `model_tests` grew the relink case (apply, one-step undo of
  both files, redo, a vanished clip skipped). Mutation-checked: a no-op `Undo`
  fails the restore checks.
- `sh scripts/haiku_syntax_check.sh`: `MainWindow`, `InspectorView`,
  `Recorder`, `Engine` all OK.
- **VM `ui_functional_tests`**: a save into `no_such_dir/x.dawproj` raises
  "Save Project"; a load of a fixture whose clip points at a file that never
  existed raises "Missing Media" and **Skip** leaves the clip byte-for-byte as
  it was; **Locate…** shows its panel, the answer is posted, and the clip's
  path becomes the found file with the undo step named "Locate Missing Media".
- VM: `build` **54/54** and `build-off` **50/50**, `ui_functional_tests` in both.
  One gotcha the run taught, now in the test: Haiku's `/tmp` is a symlink to
  `/boot/system/cache/tmp`, so the loader canonicalises media paths (and stores
  them relative to the project) -- an assertion about WHICH file a clip points
  at compares base names, not path spellings.

## What is NOT verified

| Claim | State |
| --- | --- |
| The recorder reports | **Cannot be driven**: no capture device in the harness, and a disk-full write cannot be forced from a test. Code-read: the pulse polls `ErrorCode()`, calls `StopRecording()` then `ReportError`; `FramesWritten` is what the partial take lands with. A click line for the hardware pass: record onto a full ramdisk. |
| The "Audio Device" report | Needs the device to fail (or be held by another app) while playing. The `B_ENTRY_NOT_FOUND` split is what keeps it from firing on an empty project; that path (`Play` with no clips) is exercised by the existing transport-free tests only indirectly. |
| Undo after Relink leaves the peaks stale | `RebuildPeaks` runs when the repair lands; the undo path does not rebuild (rebuilding on every undo would rescan every clip's file). The waveform of a relinked clip can therefore show the wrong envelope until the next load. Recorded, not fixed. |
