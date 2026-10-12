# Task: Preferences window (plan M3.5)

Branch `feature/m3-prefs-settings` off `master`. Spec 42
(`42-project-settings-templates.md`) covers M3.6/M3.7 and is worked on the same
branch; this file is M3.5 alone.

## What exists

- Preferences are a handful of menu radio items and one settings file:
  - `AppSettings` (`src/app/AppSettings.h/.cpp`) — kit-free `key value` text,
    host-tested in `tests/appsettings_tests.cpp`. Unknown keys are ignored and
    missing keys keep their default, so an older or newer settings file always
    loads. It already carries `bufferFrames`, `countInBars`, `metronome`,
    `monitorInput`, `lastDir`, the window frame, the panes, the export dialog's
    choices and `themeMode`.
  - `MainWindow::LoadSettings()/SaveSettings()` — the Haiku side
    (`~/config/settings/HaikuDAW/settings`); saved on quit, on the theme mode
    change and on the recent-list changes.
  - Audio menu: `Buffer Size` (radio, `MSG_BUFFER`), `Count-in` (radio,
    `MSG_COUNTIN`) and `Monitor Input` (`MSG_MONITOR_IN`). View menu: `Dark
    Mode` (`MSG_THEME_MODE`), whose move into this window was promised when
    theme modes landed (`src/ui/MainWindow.cpp` says so above the item).
- `src/ui/Theme.h` — `SetThemeScaleOverride(v)`, `ThemeScale()`: `0` = follow
  the user's font size, a positive value forces the scale. `main.cpp` already
  reads the theme mode from the settings before any window exists.
- The widget kit (`src/ui/widgets/`) — `DawButton`, `DawTextField`,
  `DawMenuField`, `DawCheckBox`, `DawSlider`, and `DawControlLook` for anything
  left stock.
- The single-instance pattern every child window already follows: keep a
  `BMessenger`, push the current state, and fall through to constructing the
  window when the messenger is dead (mixer, markers window).

## The window (`src/ui/PreferencesWindow.{h,cpp}`)

A `BWindow` on its own looper that holds **no model access** — the mixer and
the markers window are the pattern. It is given the current `PrefsView`
(all of the settings it shows), pushes values back as ONE message
(`kMsgPrefsChanged`), and reports a live change the moment a control moves
(menu choice, check box, slider release, text field committed with Enter). No
Apply button: the app's existing radio items already behaved this way, and a
button would add a second state to keep honest.

Layout (Layout Kit, in this order):

| Row | Control | Key |
|---|---|---|
| Audio output device | a disabled `DawMenuField` reading "Default Output" | none yet — see the seam below |
| Buffer size | `DawMenuField` radio (128/256/512/1024/2048 frames) | `bufferFrames` |
| Default sample rate | `DawMenuField` (44 100 / 48 000 / 96 000 Hz) | `defaultSampleRate` |
| Count-in | `DawMenuField` (Off / 1 / 2 / 4 bars) | `countInBars` |
| Metronome sound | `DawMenuField` (Click / Beep / Wood block) | `metronomeSound` |
| Metronome level | `DawSlider` 0–100 % with the value in the label | `metronomeLevel` |
| Monitor input while recording | `DawCheckBox` | `monitorInput` |
| Dark mode (this app's windows only) | `DawCheckBox` | `themeMode` |
| UI scale | `DawMenuField` (Follow the system font / 100 / 125 / 150 %) | `uiScale` |
| Default project folder | `DawTextField` + `Choose…` (`BFilePanel` in directory mode) | `projectDir` |
| Autosave every | `DawMenuField` (Off / 30 s / 1 / 5 / 10 min) | `autosaveSeconds` |

Buttons: `Close` (`B_QUIT_REQUESTED`). Title: "Preferences". Window feel
`B_FLOATING_APP_WINDOW_FEEL | B_NOT_ZOOMABLE | B_NOT_RESIZABLE`, sized to its
contents with `ResizeToPreferred()` — exactly what `ExportWindow` does.

### The audio-device seam (M5.3)

**No device enumeration is built here.** The multichannel-input package owns
`BMediaRoster` input enumeration and the output picker (plan M5.3); it adds the
control to this window. What this package leaves behind for it:

- a member `fDeviceField` (a `DawMenuField`) that is `SetEnabled(false)` and
  reads "Default Output", placed and commented as the seam;
- a block comment in `PreferencesWindow.cpp` naming M5.3 and what the picker
  has to do (list the live nodes, remember the choice in `AppSettings`, and
  hand it to the engine at the next Play);
- no code elsewhere that assumes a device choice exists.

## Settings that have to work the moment they change

- **Buffer size / count-in / monitor input** — the existing `MSG_BUFFER` /
  `MSG_COUNTIN` handlers stay (the window posts them), so the functional suite
  and the widgets drive one path. They now also `SaveSettings()`.
- **Metronome sound and level** — the engine plays the click with a hard-coded
  0.3 gain (`src/engine/Engine.cpp`, the `fMetronome.Render(...)` call) and
  `Metronome` synthesises one sound. `Metronome::Render` gains a `sound`
  argument (0 click, 1 beep, 2 wood block — three ticks with the same envelope
  and different partials, so a beat, an accent and the tempo map still come out
  the same shape), and `Engine` gains `SetMetronomeGain(float)` /
  `SetMetronomeSound(int)` (atomics, read in the callback, like
  `SetMetronome(bool)`). `Metronome` is kit-free and its tests already exist
  (`tests/metronome_tests.cpp`).
- **Dark mode** — the window posts `MSG_THEME_MODE`; `MainWindow` keeps that
  handler and its `SetThemeMode()` (it saves the preference itself). Only the
  View menu item goes away.
- **UI scale** — `SetThemeScaleOverride(scale)` (`0` = follow the system
  font), then `ApplyThemeToAllWindows()` so every window re-takes its cached
  geometry colours; and it is applied in `main.cpp` *before* the first window
  is built, which is where the theme mode is read today.
- **Default project folder** — the directory a Save/Open panel starts in when
  there is no remembered `lastDir` from the current session.
- **Autosave interval** — the fixed 30 s `BMessageRunner` is rebuilt with the
  chosen interval; `0` stops it (no runner at all).

## AppSettings (kit-free)

New keys, one line each, all optional on load (an older file keeps the
defaults — that is the format's contract):

```
defrate <44100|48000|96000>   defaultSampleRate, 48000
metsnd  <0|1|2>               metronomeSound,    0 (click)
metlvl  <0..1>                metronomeLevel,    0.30
uiscale <0|..>                0 = follow the system font (Theme.h's default)
projdir <path to end of line> default project folder, ""
autosv  <seconds>             autosaveSeconds,   30  (0 = off)
```

`Sanitize()` runs at the end of `Deserialize` (and before `Serialize` writes
nothing new): it snaps every numeric preference to the set the window offers,
because a hand-edited or truncated file must not be able to leave the app with
a `bufferFrames` of 0 (a division in the audio path) or a negative level. The
allowed sets live in `AppSettings.h` beside the fields so the window's menus
are built from the same table a load accepts.

## Files

- `src/app/AppSettings.{h,cpp}` — the six keys, the choice tables, `Sanitize()`.
- `tests/appsettings_tests.cpp` — round trip, legacy file (no new keys),
  unknown key, and the sanitiser.
- `src/engine/Metronome.{h,cpp}` + `src/engine/Engine.{h,cpp}` — the click's
  sound and gain.
- `tests/metronome_tests.cpp` — the three sounds differ and still accent bar 1.
- `src/ui/PreferencesWindow.{h,cpp}` — new; joins `daw_ui` in `CMakeLists.txt`.
- `src/ui/MainWindow.{h,cpp}` — `MSG_PREFERENCES` (File menu), the
  single-instance window, `PrefsView` in/out, `ApplyPrefs()`, the Audio menu
  loses its radio submenus (it keeps the metronome quick toggle), the View menu
  loses Dark Mode, `LoadSettings`/`SaveSettings` carry the new keys.
- `src/main.cpp` — apply the UI scale with the theme mode, before the first
  window.
- `tests/ui_functional_tests.cpp` — open it, drive it, `Shot("preferences")`.

## Verification

- Host: `cmake --build build-host`, `ctest --test-dir build-host` all green
  (the new AppSettings and Metronome behaviour is host-tested, mutated, and
  the mutation pasted into the record).
- `sh scripts/haiku_syntax_check.sh` 0 FAIL (it compiles
  `src/ui/PreferencesWindow.cpp` and `tests/ui_functional_tests.cpp`).
- On the VM (the orchestrator runs it): the window on screen in both theme
  modes, plus the functional flow that changes a value, proves the main window
  took it and that it survives `ReloadSettings()`.
