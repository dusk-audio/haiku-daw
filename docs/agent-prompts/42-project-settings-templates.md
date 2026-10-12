# Task: Project Settings + New Project templates (plan M3.6, M3.7)

Same branch as spec 41 (`41-preferences.md`, M3.5); this file is M3.6 and M3.7.

## M3.6 — Project Settings window

### What exists

- `Project` (`src/model/Project.h`): `sampleRate`, `tempoBPM`, `timeSig`,
  `tempoMap` (the authoritative tempo/meter list), `transport`, `markers`.
  Serialized by `ProjectIO` (`sampleRate` at Save, `tempo`/`timesig` +
  the `tempochange`/`meterchange` lines). **There is no project `notes` field.**
- `SetTempoCommand` already exists and is what the transport's BPM field
  executes. There is **no** meter command and no sample-rate command; the
  ruler's tempo/meter menu writes the map directly (a known M0.5-shaped gap in
  `TimelineView`, not this package's to fix).
- Frames are samples **at the project rate**: clip `startFrame`/`lengthFrames`/
  `sourceOffset`, MIDI notes/events, markers, the loop/punch points and the
  tempo map's change frames are all project-rate frames.
- `src/engine/Resampler` (kit-free, linear) and the format factories
  (`src/engine/AudioFormats.h`) + `WavWriter`/`WavSource` are host-testable, so
  an offline resample of a media file is host-testable too.

### The window (`src/ui/ProjectSettingsWindow.{h,cpp}`)

Own looper, no model access (the Preferences window's shape). It is given the
current values, pushes back ONE message, and each control applies through
`CommandStack` in `MainWindow`:

| Row | Control | Command |
|---|---|---|
| Sample rate | `DawMenuField` (44 100 / 48 000 / 96 000 Hz) | `SetSampleRateCommand` |
| — | `DawCheckBox` "Resample the media to the new rate" | …its second argument |
| Tempo | `DawTextField` (BPM) | `SetTempoCommand` (exists) |
| Meter | `DawMenuField` (4/4, 3/4, 2/4, 6/8, 5/4, 7/8) | `SetMeterCommand` (new) |
| Notes | `DawTextField` (single line, committed with Enter) | `SetProjectNotesCommand` (new) |

The sample-rate row is the only one that asks anything else: it tells the user
what changing the rate does (the media can be rewritten at the new rate, the
timeline rescales either way) in a `BStringView` hint under it, because that is
a big, undoable edit to the whole project.

### Model work

- **`Project::notes`** (`std::string`) — a new optional `notes <quoted>` line in
  `ProjectIO` (append-only: written only when non-empty, so a project that never
  uses it stays byte-identical), plus `SetProjectNotesCommand`.
- **`SetMeterCommand(num, denom)`** — `timeSig` **and** `tempoMap.SetMeterAt(0,
  …)` together (they are the same fact stored twice; `Project::Clear` resets
  both), with both captured for undo.
- **`SetSampleRateCommand(newRate, resampleMedia)`** — changes
  `p.sampleRate`/`tempoMap.sampleRate` and **rescales the timeline** by
  `newRate/oldRate`: every project-rate frame in the model moves with it, so a
  bar stays a bar and a clip stays where the user put it. The frame scaling
  lives in a kit-free `src/model/ProjectTimebase.{h,cpp}`
  (`ScaleProjectTimebase(Project&, double ratio)`) so it can be tested against
  every frame-bearing field directly. Undo restores a snapshot of the project
  taken when the command first ran (a `Project` copy — the model is plain data,
  and this is not the audio thread), so the undo is exact whatever the ratio
  rounds to.
- **Media resample** (`src/model/ProjectResample.{h,cpp}`, kit-free): when
  `resampleMedia` is set, every distinct clip source that this build can decode
  is written as a new `<stem>-<rate>.wav` (32-bit float, next to the original)
  through `IAudioSource` + `Resampler` + `WavWriter`, and the clips are
  repointed to it inside the same command. A file that cannot be decoded or
  written is left alone (the clip keeps the old file — the engine already
  plays any source rate at the right pitch, so nothing is broken by it).

### M3.7 — New Project dialog with templates

- **`data/templates/*.dawproj`** — `empty.dawproj`, `band.dawproj` (8 audio
  tracks into a bus, named and coloured, 4/4 at 120), `producer.dawproj` (a
  MIDI track with an instrument, an audio track, a bus and a return). No
  clips and no media: a template is a *starting point*, so a missing media file
  can never be what greets a user who just asked for a new project.
- **`src/app/ProjectTemplates.{h,cpp}`** (kit-free, in `daw_model`):
  - `FindTemplatesDir(candidates)` — the first candidate that exists and holds
    at least one `.dawproj`; `""` when there is none (never a crash, never a
    dialog that cannot be answered: with no templates the dialog offers Empty
    and says so).
  - `ScanTemplates(dir)` — `TemplateInfo { id, name, description, path }`, the
    known ids first (empty, band, producer), then any other `*.dawproj`
    alphabetically, so dropping a file into `data/templates` is enough to
    offer it.
  - `LoadTemplate(info, Project&)` — `ProjectIO::Load` + the template's own
    declared content; the SESSION rate wins afterwards (the same rule
    `MainWindow::NewProject` has always had: the rate belongs to the device).
- **`src/ui/NewProjectWindow.{h,cpp}`** — own looper, Layout Kit: a
  `DawMenuField` of the templates, a `BStringView` describing the marked one,
  and `Cancel` / `Create` (`Create` is the default button). With no template
  directory it shows the Empty row plus a hint saying where templates are
  looked for.
- **`MainWindow`**: `MSG_NEW_PROJECT` opens the dialog (single instance);
  `kMsgNewProjectChosen` carries the template id, and the existing
  confirm-discard → reset → `ShowReplacedProject` flow runs after it.
- **CMakeLists.txt** — the first install/copy rule in this project:
  - a build-tree copy so a build-directory `daw` (and the functional suite)
    finds `data/templates` beside itself (`<build>/data/templates`, with a
    declared dependency so editing a template re-copies it);
  - an `install(DIRECTORY data/templates …)` to the app's data directory
    (`$dataDir/haiku-daw/templates`), and the one line in
    `haiku-daw-1.0.0.recipe`'s `INSTALL()` that puts it there in the package —
    the recipe copies the binary by hand today, so a CMake `install()` alone
    would never run.
  - lookup order at runtime: `DAW_TEMPLATES_DIR` (tests and a dev override),
    then `<app dir>/data/templates`, `<app dir>/../data/templates` (build
    tree), then the installed data directory.

### Files

- `src/model/Project.h` (`notes`), `Commands.{h,cpp}`, `ProjectIO.cpp`,
  `ProjectTimebase.{h,cpp}`, `ProjectResample.{h,cpp}` — kit-free.
- `src/ui/ProjectSettingsWindow.{h,cpp}`, `src/ui/NewProjectWindow.{h,cpp}` —
  new; both join `daw_ui` in `CMakeLists.txt`.
- `src/ui/MainWindow.{h,cpp}` — the two menu items, both single-instance
  windows, the apply paths and the new-project flow.
- `data/templates/*.dawproj`, `CMakeLists.txt`, `haiku-daw-1.0.0.recipe`.
- `tests/project_settings_tests.cpp` (new target), `tests/ui_functional_tests.cpp`.

### Verification

- Host: the new model behaviour (meter, notes and their IO/compat, the timebase
  scale over every frame field, the sample-rate command's undo, the media
  resample's length/frequency, template discovery with a missing directory, and
  every shipped template parsing into the shape it promises) is host-tested and
  mutation-checked; the mutations go in the record.
- `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- On the VM (the orchestrator): the Project Settings window and the New Project
  dialog on screen, the template flows driven by posted message, and the
  install/copy rule checked by running the suite from the build tree.
