# M3.5 Preferences window — PR record

Branch `feature/m3-prefs-settings`. Spec: `41-preferences.md`; M3.6/M3.7 are on
the same branch and have their own record (`42-project-settings-templates-PR.md`).

## Base correction (read this first)

The worktree was cut from `origin/docs/1.0-decisions` (`785cd5a`), which is 364
commits BEHIND `master` (`85b19b1`) and is an ancestor of it. Everything this
package touches — `src/ui/widgets/`, `src/ui/Theme.h`, the Audio menu's radio
items, `docs/PLAN_1.0.md`, `AGENTS.md`, `docs/UI_GUIDELINES.md` — exists only on
`master`, so the branch was fast-forwarded to it before any code was written
(`git reset --hard master` in this worktree; no shared ref touched, the branch
had no commits of its own, `785cd5a` is an ancestor so nothing was lost). The
orchestrator approved this in writing before it happened. All five agent
worktrees were created from the same stale ref; two peers were re-based the same
way.

## What changed

### The settings (kit-free, `src/app/AppSettings.{h,cpp}`)

Six optional keys, one `key value` line each, so an older file keeps the
defaults and a newer one is ignored rather than fatal:

```
defrate <48000>   the rate a NEW project starts at
metsnd  <0>       the click's sound (0 click, 1 beep, 2 wood block)
metlvl  <0.30>    the click's level, 0..1
uiscale <0>       0 = follow the user's font size (Theme.h's own default)
projdir <path>    the folder a first Save starts in (rest of the line)
autosv  <30>      crash-recovery interval in seconds; 0 = off
```

The values the window offers live in `AppSettings.h` as tables
(`kBufferFrameChoices`, `kSampleRateChoices`, …), so the menus and what a load
accepts cannot drift. `AppSettings::Sanitize()` — called at the END of
`Deserialize` — snaps every one of them to something the app can run with: a
buffer of 0 or 500 frames becomes 512, a negative level becomes 0, an
out-of-range sound index becomes the click, an out-of-range scale is clamped,
and 0 stays the sentinel it is. `buffer 0` used to reach the audio path.

### The window (`src/ui/PreferencesWindow.{h,cpp}`, new)

A `BWindow` on its own looper holding **no model access** — the mixer/markers
pattern. `PrefsView` is one struct in both directions: MainWindow pushes the
current values in (`kMsgPrefsValues`), any control that moves posts the whole
set back as ONE message (`kMsgPrefsChanged`), and `MainWindow::ApplyPrefs()` is
the only place a preference is applied or saved. Rows: audio output device
(disabled, see the seam), buffer size, default sample rate, count-in, metronome
sound, metronome level (slider + readout), monitor input, dark mode, UI scale,
default project folder (text field + `Choose…` folder panel), autosave
interval; one `Close` button; `ResizeToPreferred()` like `ExportWindow`.

**The M5.3 seam.** No device enumeration is built anywhere in this package. The
row is a disabled `DawMenuField` reading "Default Output" with a hint line
saying where the picker arrives, a block comment in
`PreferencesWindow.cpp` naming M5.3 and what the picker must do, and a
`DeviceField()` accessor so the follow-up has an obvious handle. Nothing else in
the package assumes a device choice exists.

### What moved out of the menus

- **Audio** keeps the quick toggles only: it now holds the **Metronome** item
  (moved from View — it is the one toggle that belongs to audio), and loses the
  Buffer Size and Count-in radio submenus and the Monitor Input item.
- **View** loses Dark Mode (promised when theme modes landed; the comment in
  `MainWindow.cpp` said "it moves into the Preferences window when M3.5 builds
  one"). `MSG_THEME_MODE` is unchanged — the window's check box posts it — so
  `TestThemeMode` still drives the same path.
- `MSG_BUFFER`, `MSG_COUNTIN` and `MSG_MONITOR_IN` all still work and now land
  on `ApplyPrefs()`, the same funnel the window uses: one path, not two.

### Applied live

- **Buffer size / count-in** — applied at the next Play, as the menu items were.
- **Metronome sound and level** — the hard-coded `0.3f` at
  `Engine.cpp`'s `fMetronome.Render(...)` call is now an atomic pair
  (`SetMetronomeGain`/`SetMetronomeSound`), and `Metronome::Render` takes the
  sound (three timbres: the same 30 ms envelope and the same bar accent, a
  different base tone and one partial). `TransportController` carries both next
  to `fMetronome` and programs them wherever an engine is built or re-programmed,
  so a change while stopped is heard on the next Play.
- **Monitor input** — live, exactly as the Audio menu's toggle did it (timeline
  "I" lamp, a running take's recorder, the engine).
- **Dark mode** — `SetThemeMode()`, which also saves (as before).
- **UI scale** — `SetThemeScaleOverride()` + a full theme walk; **and it is
  applied in `main.cpp` before the first window is built**, next to the theme
  mode, which is what the plan asked for. `LoadSettings()` deliberately does not
  re-apply it (a reload would fight the suite's own 150% passes).
- **Autosave** — `StartAutosave(seconds)` replaces the fixed 30 s runner; 0
  builds no runner at all.

## Verification (host, on this machine)

- `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` → exit 0;
  `cmake --build build-host -j2` → exit 0.
- `ctest --test-dir build-host` → **100% tests passed out of 70**.
- `sh scripts/haiku_syntax_check.sh` → **80 OK, 4 FAIL**. The four are
  `src/engine/{FlacSink,FlacSource,VorbisSink,VorbisSource}.cpp`, whose
  third-party headers (`FLAC/stream_encoder.h`, `ogg/ogg.h`) are not in the
  cross sysroot on this host; `git diff master` shows all four byte-identical to
  master, i.e. the failures are pre-existing and environmental, not from this
  branch. Everything this branch adds or edits is OK, including
  `PreferencesWindow.cpp`, `MainWindow.cpp`, `main.cpp` and
  `tests/ui_functional_tests.cpp`.
- New/changed host tests: `appsettings_tests` 128 checks, `metronome_tests`
  22 checks — 0 failures both.

### Mutations (each broken, watched to fail, restored)

| Mutation | Caught by |
|---|---|
| `Sanitize()` call removed from `Deserialize` | `appsettings_tests`: 15 failures (buffer 0/500, the clamps, the sound index) |
| the `defrate` key renamed in `Serialize` | `appsettings_tests`: the round trip (`p2.defaultSampleRate == 96000`) |
| the beep timbre made identical to the click | `metronome_tests`: `differ(ticks[0], ticks[1])` |
| `Metronome::Render` ignoring its `gain` argument | `metronome_tests`: `halfOfClick` |

## Left for the VM (the orchestrator's pass — I cannot run it)

- The screenshot pass (`DAW_UI_SHOTS`) in **both** theme modes and at 150%, on
  every shot of the Preferences window (`Shot("preferences")` is in
  `TestPreferences`), then the fix-what-you-see loop.
- `ui_functional_tests` on the VM: `TestPreferences` opens the window from
  `MSG_PREFERENCES`, proves it is single instance, changes the count-in, buffer,
  scale and dark mode, checks the main window took each (its two new
  `*ForTest()` accessors), and reloads the settings file to prove the choices
  were persisted. It restores every value it touched before it finishes.
- `build-off` (no LV2) and `build` builds.

## Notes and deviations

- **Metronome moved View → Audio.** The brief says the Audio menu keeps quick
  toggles only; with the three preferences gone, the metronome is the toggle
  that belongs there. Nothing in the suite depends on the item's location.
- Two test accessors were added to `MainWindow`'s public block
  (`BufferFramesForTest`, `CountInBarsForTest`) because both values live in
  controllers the suite cannot read; they are `const` read-only getters.
- The Audio menu's ids were kept rather than deleted: the window is not the only
  path a preference can arrive by (a test posts them), and two paths that
  disagree is the bug this funnel exists to prevent.
