# T1 theme modes — PR record

Branch `feature/theme-modes` off `master` (`6d88063`). Spec: `25-theme-modes.md`.

## What changed

### The app no longer writes a system colour

`DawApplication::ApplyThemeColors()` — 17 `set_ui_color` calls — is gone. That
call rewrites the **user's** Appearance for every application on the machine
(Haiku saves it), so Tracker and everything else stayed recoloured after the DAW
quit, and the always-dark look was not the default Marc wants.

What is left is per-process state the app owns: `be_control_look` (Haiku's
control-look pointer, one per process) and the app's own view colours.

### The palette became a pure function of the base colours (kit-free)

- New `src/app/ThemePalette.{h,cpp}` in `daw_model`: `ThemeColor`,
  `Tint`/`Mix` (Haiku's `tint_color` maths, reimplemented so a Linux host can
  test it), WCAG `Luminance`/`ContrastRatio`, `LabelOn`, `EnsureContrast`,
  `ThemeBase` (the 17 system colours as data), `ThemeTokens` (one per `Col*()`
  accessor) and `DeriveTokens(base, mode)`.
- `ThemeMode { System, Dark }`; `AppSettings.themeMode` round-trips in the
  kit-free settings text (`theme system|dark`), defaults to System, and an
  unknown value stays System.
- `src/ui/Theme.h` is the adapter: every `Col*()` name and signature is
  unchanged (≈300 call sites), and each returns the active theme's token. The
  meter thresholds, `TrackColor`, `Themed()` and the font tokens are untouched.

### Dark mode is today's palette, System mode is the user's

`DeriveTokens(…, Dark)` returns exactly the literals `Theme.h` had before (the
host test asserts each one, so the dark look cannot drift while this lands).
System mode derives from `ui_color()`: panel → background/chrome, panel text →
text, control → header strip, list background → lanes, document → the recessed
wells, keyboard-navigation → accent, with `tint`/`mix` for the variants and
`IsDark(panel)` choosing which way a variant goes, so one rule serves a light
and a dark Appearance. The vivid colours that read on both — the clip blue, the
waveform white, the M/S/R lamps, the meter colours, the per-track palette, the
ruler's tempo amber (pulled towards the text colour by `PanelAccent`) — stay
constants; the accent (the user's own) is pushed until it contrasts with the
panel, and every label on a filled box uses `LabelOn` instead of a fixed
brightness threshold.

### The look follows the mode (and so does everything cached)

- `InstallControlLookForMode(mode)`: Dark installs `DawControlLook`, System
  puts back the pointer the kit already had (never a private Haiku class we
  construct ourselves).
- `ThemeAware` + `ApplyThemeToAllWindows()`: on a mode switch, or on
  `B_COLORS_UPDATED` in System mode, every open window is walked, the views
  that cached a colour re-take it, everything else is invalidated.
- The kit draws through the look now, so a kit button and a stock button cannot
  drift apart: `DawButton`/`DawToggle` (a lit toggle keeps its own lamp colour
  — the look cannot express "this means REC"), `DawSlider` (trough, thumb, hash
  marks), `DawCheckBox`, `DawMenuField`, `DawTextField`. `DawKnob`, the fader,
  the VU meter and the timeline's lamps stay custom-drawn from tokens, and the
  look grew the hover state the kit's controls can no longer paint themselves.
- The transport's tempo field became a `DawTextField` and the master slider's
  bar colour a token: a stock text control's well is the system document colour,
  which is a white box in a dark window now that the app may not rewrite it.
- `ThemedMenuItem`: a menu item's label colour comes from the system colour
  table (`BMenuItem::_HighColor`), which the app may not write — so every menu
  item in the app (92 of them) draws its own label and shortcut in the theme's
  text colour, and the menu bar's titles are built the same way.
- Stock windows (file panels, alerts) are themed by `ThemeStockWindow` /
  `ThemedAlertWindow` — in Dark mode the panel colour and the wells go on
  explicitly, in System mode they go back to `SetViewUIColor(...)`, the
  system's own. A panel builds its views after `Show()` returns, so the pass
  runs again a moment later (`MSG_THEME_STOCK`); an alert is themed from its
  own `Show()`, before `Go()` starts waiting.

### View menu

View ▸ **Dark Mode** toggles it, saves it, and applies it live. It moves to the
Preferences window when M3.5 builds one.

## Verification

| Check | Result |
|---|---|
| Host build (exit 0) + `ctest --test-dir build-host` | **53/53** |
| ASan+UBSan `b-asan` | **53/53** |
| `scripts/haiku_syntax_check.sh` | **0 FAIL**, 66 files |
| VM `build` ctest | **55/55** |
| VM `build-off` ctest | **51/51** |
| `ui_functional_tests` | **220 checks, 0 failures** (was 210) |
| Screenshot pass, System mode (Haiku's shipped light Appearance) | 26 shots reviewed |
| Screenshot pass, Dark mode | 26 shots reviewed |

New tests:

- `tests/theme_palette_tests.cpp` (host, 79 checks): Dark mode's tokens equal
  the literals `Theme.h` shipped; System mode's rules keep text ≥ 4.5:1 on a
  light *and* a dark system panel, the playhead reads on the panel, the grid is
  visible without being a stripe, the alternate lane is a hint not a stripe;
  `LabelOn` on the lamp colours (≥ 4.5) and on a clip (≥ 3, large text); the
  colour maths clamps; the settings text.
- `tests/appsettings_tests.cpp`: `themeMode` round-trip, missing key → System,
  garbage value → System.
- `tests/ui_functional_tests.cpp`: `TestThemeMode` flips the mode through the
  View menu message, checks that the window's own background **and the
  timeline's** (a sibling of the menu bar, not its child) actually changed, that
  the control look follows the mode, and — the check that fails if anyone
  reintroduces `set_ui_color` — that `ui_color(B_PANEL_BACKGROUND_COLOR)` never
  moves. The run's mode comes from `DAW_UI_THEME` (`dark`, `system`, or `light`
  for System mode on Haiku's shipped light colours), so the whole pass runs
  twice; `light` exists so the light-panel case can be reviewed on a machine
  whose own Appearance is something else, without changing that machine.

Mutation checks (each broken on purpose, seen to fail, restored):

1. `t.accent` taken straight from `ui_color` instead of through
   `EnsureContrast` → `theme_palette_tests`: "dark panel: the accent on the
   panel: contrast 1.44 < 3.00".
2. `LabelOn` always returning white → 5 checks fail, e.g. "play lamp label:
   contrast 2.22 < 4.50".
3. `AppSettings::Deserialize` ignoring the `theme` line → `appsettings_tests`:
   "odd.themeMode == ThemeMode::Dark" fails.

## What looking at the screen caught

The screenshot pass is what found the two bugs that made this worth doing:

1. **The live switch changed the state and not the screen.** `ApplyThemeToAllWindows()`
   walked `window->ChildAt(0)`; a window built with the Layout Kit has one child
   per top-level item, so the main window's menu bar was refreshed and the panes
   were not. Every check passed — they read colours, not pixels — while the shots
   showed the old mode. Fixed with `ApplyThemeToWindowViews()`, and
   `TestThemeMode` now reads a pane (the timeline) as well as the window.
2. **Menus were unreadable in Dark mode.** `BMenuItem` takes its pen from the
   system colour table, which the app may no longer write: black text on the
   dark menu bar. Fixed with `ThemedMenuItem` (see above).

Also fixed from the shots: the alert and file-panel light islands, the black
per-track meter bar down every header on a light panel (`Rgb(16,16,20)` →
`ColLcd()`), the ruler's tempo/meter markers and the automation curve
(`PanelAccent`), and the test's own probe window, which framed a dark panel in
the system's light panel colour.

## Fixed on the way past

`scripts/haiku_syntax_check.sh` could not compile anything that includes
`<Screen.h>` (`Accelerant.h` is under `os/add-ons/graphics/`, which was not on
its include path) — so `EffectsWindow.cpp` always "failed" on a healthy tree.
It now also finds the LV2 headers (lilv/lv2 via pkg-config, through a one-entry
shim so the system include root is not dragged into libstdc++), and its default
file list is all of `src/**.cpp` plus `tests/ui_functional_tests.cpp`
(66 files, 0 FAIL) instead of ten hand-picked ones.

## For Marc

- The shots are in `docs/agent-prompts/shots-25/{light,dark}/` (26 each, same
  order as the tests). The `system` pair from the earlier round was dropped: the
  VM's Appearance changed under the run at 08:59 (and its wallpaper with it),
  which is exactly the kind of thing System mode is now at the mercy of.
- **This VM's system colours already hold the DAW's old dark palette** — written
  by the pre-T1 build's `set_ui_color` and saved by Haiku. That is the bug this
  task fixes; nothing in the new code writes them. To put the VM (or the
  hardware box) back on stock colours: **Preferences ▸ Appearance ▸ Colors ▸
  Defaults**. Not done here on purpose.
- Click list, on the hardware box: switch View ▸ Dark Mode while a plugin
  editor and the mixer are open (they should follow), change Appearance while
  the app runs in System mode (it should follow), and confirm Tracker is
  untouched after quitting the DAW.
