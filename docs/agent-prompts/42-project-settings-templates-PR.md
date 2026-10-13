# M3.6 Project Settings + M3.7 New Project templates — PR record

Same branch (`feature/m3-prefs-settings`) and the same base correction as
`41-preferences-PR.md`; specs `42-project-settings-templates.md` and
`41-preferences.md`.

## M3.6 — Project Settings

### Model (kit-free, host-tested)

- **`Project::notes`** — free text, an optional `notes "<quoted>"` line in
  `ProjectIO` (append-only: written only when non-empty, so a project that never
  sets it saves the bytes it always did, and a file from before the field loads
  with no notes rather than garbage). Cleared by `Project::Clear()`.
- **`SetMeterCommand(num, denom)`** — writes `timeSig` AND the tempo map's
  frame-0 meter, because they are the same fact stored twice (the map is what
  the engine and the ruler read; `timesig` is what serializes). Undo restores
  both. A nonsense meter (0/0) is clamped instead of stored.
- **`SetProjectNotesCommand(notes)`** — an ordinary edit: dirty, undoable.
- **`ScaleProjectTimebase(Project&, ratio)`** (`src/model/ProjectTimebase.{h,cpp}`,
  new) — frames ARE samples at the project's rate, so changing the rate is a
  rescale of the whole timeline. Every frame-bearing field: the transport's
  playhead/loop/punch, markers, the tempo AND meter change anchors, clips
  (start/length/sourceOffset/fades), MIDI regions (their own frames, their
  notes' and CC events' frames), the pre-freeze copies a frozen track carries,
  and the automation breakpoints (`gainAuto`, `panAuto`, `fxAuto`). Non-time
  fields (ids, gains, colours, lane heights, tempo BPM) are untouched.
- **`SetSampleRateCommand(newRate, resampleMedia)`** — the media first (each
  DISTINCT source is written as a new `<stem>-<rate>.wav` 32-bit float file
  beside the original through `ResampleMediaFile`, and the clips that used it
  follow; a file that cannot be decoded or written keeps its clip where it was,
  which cannot break anything because the engine plays any source rate at the
  right pitch), then the timeline rescale, then the rate itself. Undo restores a
  `Project` copy taken on the first `Do()` — exact whatever the ratio rounds to;
  the converted files stay on disk and a redo overwrites them.
- **`ResampleMediaFile`** (`src/model/ProjectResample.{h,cpp}`, new) — the file
  work, through the existing factories (`OpenAudioSource`, `Resampler`,
  `WavWriter`), so it is host-testable: a 1 s 440 Hz tone at 48 k in, a 1 s tone
  at 96 k out, zero crossings unchanged.

### The window (`src/ui/ProjectSettingsWindow.{h,cpp}`, new)

Own looper, no model access. Sample rate (menu built from `kSampleRateChoices`
plus the project's own rate when it is not among them), "Resample the media to
the new rate" (disabled and cleared when there is no media), a hint saying what
a rate change does, tempo (text field, clamped 20–300 like the transport's),
meter (menu of the six common signatures plus the project's own), notes (a
`BTextView` in a scroll view — a text FIELD would silently collapse the
newlines a project already has), and `Close` / `Apply`. Apply, not
apply-as-you-type: the rate is a whole-project edit and a tempo field cannot
commit per keystroke.

`MainWindow::ApplyProjectSettings()` turns the DIFFERENCES into one
`MacroCommand("Project Settings")`, so a change to four fields is one Edit-menu
step. It then updates the tempo field, rebuilds the peaks when clips may have
been repointed, invalidates the timeline, reloads the running engine, updates
the title and pushes the applied values back to the window. File > Project
Settings… opens it, single-instance (mixer/markers rule).

## M3.7 — New Project with templates

- **`data/templates/`** — `empty.dawproj`, `band.dawproj` (8 audio tracks, the
  drums routed to a bus), `producer.dawproj` (2 MIDI tracks with instruments, 3
  audio tracks, a reverb bus with the vocal and drums sending to it). Ordinary
  project files: no clips and no media, so a template never greets a user with a
  missing-file walk.
- **`src/app/ProjectTemplates.{h,cpp}`** (kit-free) — `TemplateCandidates`
  (environment, then the app's own directory, then the installation's data
  directory), `FindTemplatesDir` (the first candidate that holds at least one
  `.dawproj`; `""` when none does), `ScanTemplates` (known ids first in their
  own order, then any other file alphabetically), `EmptyTemplate()` (the
  built-in row, no file) and `LoadTemplate`.
- **`src/ui/NewProjectWindow.{h,cpp}`** — the dialog: a template menu, the
  marked template's description, and Cancel / Create. It ALWAYS has an Empty row
  (the shipped file when there is a template directory, the built-in one when
  there is not), and with no template files at all it says where it looked
  instead of showing a dead end. So a missing directory is a non-event, which is
  what the brief demanded.
- **MainWindow** — `MSG_NEW_PROJECT` opens the dialog; the choice arrives as
  `kMsgNewProjectChosen` (id + path); `NewProjectFrom()` does the
  unsaved-changes prompt, then either the template load or an empty session. A
  template that cannot be read is reported and the user gets the empty session —
  never a half-loaded one. The session's rate is the **default-sample-rate
  preference** (M3.5): a template brings its tracks, routing and tempo, not its
  rate.
- **CMakeLists.txt — the project's first copy/install rule.** The three
  templates are declared build outputs copied to `<build>/data/templates` (a real
  `add_custom_command` with `DEPENDS`, not a POST_BUILD copy, so editing one
  re-copies it), `daw` and `ui_functional_tests` depend on that target, and
  `install(DIRECTORY data/templates DESTINATION data/haiku-daw)` puts them in
  the package's data directory. `haiku-daw-1.0.0.recipe`'s `INSTALL()` gained the
  matching copy, because that recipe copies the binary by hand and never runs
  `make install`.

## Verification (host, on this machine)

- Build exit 0; `ctest --test-dir build-host` → **100% of 70 tests pass**.
- `sh scripts/haiku_syntax_check.sh` → 80 OK, 4 FAIL, all four pre-existing
  (FLAC/Ogg third-party headers missing from the cross sysroot; those files are
  byte-identical to master). Everything this package adds is OK, including both
  new windows and `tests/ui_functional_tests.cpp`.
- New host suite `project_settings_tests`: **138 checks, 0 failures** — the
  meter/notes commands and their round trip and compat (no `notes` line, an
  unknown future line), the rescale field by field and its exact round trip,
  the sample-rate command's undo/redo/no-op/refusal, the media conversion (a
  440 Hz tone's length and frequency), the resample inside the command, and a
  whole-project save/load.
- New host suite `project_templates_tests`: **86 checks, 0 failures** —
  discovery priority, a missing directory, an empty directory, a candidate that
  names a file, the row order, dropped-in files, directories and non-projects
  ignored, the failure paths, and **the three shipped templates parsing into the
  shape they promise** (Band: 8 audio + 1 bus, 3 routed to the bus, all named;
  Producer: 2 MIDI with instruments, 3 audio, 1 bus, 2 sends pass). That last
  part is why the templates cannot silently rot: the suite reads them from the
  source tree on every run.

### Mutations (each broken, watched to fail, restored)

| Mutation | Caught by |
|---|---|
| `ScaleProjectTimebase` skips `freezeClips` | `freezeClips[0].lengthFrames == 48000` (the frozen copy stays behind in time) |
| … skips markers | `p.markers[0].frame == 24000` |
| `SetMeterCommand` writes only `timeSig`, not the map | 3 failures (`Meters().front().num/denom`, including the clamp) |
| `SetSampleRateCommand` skips the rescale | 5 failures (clip start/length, the save round trip) |
| … and its Undo restores only the rate | the undo/redo checks |
| `ProjectIO::Save` renames the `notes` key | the notes round trip (2 checks) |
| `ResampleMediaFile` resamples to the SOURCE rate | `TotalFrames() >= 95000 && <= 96500` (both the direct and in-command paths) |
| `FindTemplatesDir` accepts a directory with no `.dawproj` | 3 discovery checks (the empty and missing directories win) |
| `LoadTemplate` reports success for a missing file | the failure-path checks |
| `band.dawproj` loses its bus track | `p.Tracks().size() == 9` |

## Left for the VM (the orchestrator's pass)

- Screenshots of both new dialogs in both theme modes and at 150%
  (`Shot("project-settings")`, `Shot("project-settings-applied")`,
  `Shot("new-project")`, `Shot("new-project-band")`).
- `ui_functional_tests` on the VM: `TestProjectSettings` (opens, single
  instance, shows the project's real values, applies tempo+meter+notes as ONE
  edit — `UndoName() == "Project Settings"` — and undoes it; then a rate change
  and its exact undo), `TestNewProjectTemplates` (the three rows, single
  instance, Band creating a 9-track session, and a bogus `DAW_TEMPLATES_DIR`
  still leaving an answerable dialog), and the updated
  `TestFileMenuFlows` (New now goes through the dialog before the
  unsaved-changes prompt).
- The build-tree copy rule itself: run the suite from a build directory on the
  VM and confirm the dialog finds `data/templates` beside the binary.
- `build-off` and `build` suites.

## Notes

- The ruler's own tempo/meter menu (`TimelineView`) still writes the map
  directly; that is the M0.5-shaped gap the plan lists elsewhere, untouched here.
- `MainWindow::NewProject()` is gone, replaced by `ShowNewProjectWindow()` +
  `NewProjectFrom()`; the header was updated in place.
