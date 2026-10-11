# Task: theme modes — the app never changes the OS; native look by default, dark mode by choice (T1)

Branch `feature/theme-modes` off `master`. Decision by Marc, 2026-10-10
(`docs/agent-prompts/24-continue-1.0-ENTRY.md` §3.1, `docs/UI_GUIDELINES.md` §5):
the app never writes system settings, the default look is native Haiku, and a
dark mode in Preferences styles the DAW's own windows only.

## Codebase orientation (read before coding)

- **The bug.** `DawApplication::ApplyThemeColors()` (`src/main.cpp:129`) calls
  `set_ui_color` for 17 system colours. `set_ui_color` writes the *system*
  colour map (`src/kits/interface/InterfaceDefs.cpp`, Haiku: the values are
  saved by the app_server), so Tracker and every other app stay recoloured
  after the DAW quits — and the dark look it applied is now the wrong default.
  All 17 calls go; nothing replaces them.
- **Where colour comes from today.** `src/ui/Theme.h` is the token header:
  `ColBackground()`, `ColLane()`, `ColLaneAlt()`, `ColRuler()`, `ColGrid()`,
  `ColClip()`, `ColClipBorder()`, `ColWave()`, `ColText()`, `ColTextDim()`,
  `ColPlayhead()`, `ColHeader()`, `ColHeaderHi()`, `ColAccent()`,
  `ColAudioAccent()`, `ColMidiAccent()`, `ColBtnOff()`, `ColBtnBorder()`,
  `ColBtnText()`, `ColMute()`, `ColSolo()`, `ColRec()`, `ColMon()`,
  `ColPlay()`, `ColKnobBody()`, `ColKnobOutline()`, `ColChrome()`,
  `ColChromeHi()`, `ColLcd()`, `ColLcdText()`, `TrackColor(int)`,
  `MeterColor(float)`. Every literal palette value lives here and nowhere else —
  ~300 call sites across `src/ui/`, `src/main.cpp` and the widget kit. Those
  names stay; only what they return becomes mode-dependent.
- **What draws with the tokens.** Fully custom-drawn surfaces (`TimelineView`,
  `PianoRoll`, `MeterView`, `InspectorView`, `TransportBar`, `MixerWindow`,
  `Widgets.h`'s knob/fader/VU/button helpers) call `Col*()` at draw time, so
  they follow a mode change for free once the tokens change. What does NOT
  follow for free is anything cached: a window's `SetViewColor(...)` in its
  constructor, and the kit controls' `AttachedToWindow` colour adoption.
- **The kit** (`src/ui/widgets/`): `DawControl` (base: hover/press/focus,
  wheel, tooltip, `AdoptPanelColors` at `DawControl.h:25`), `DawButton` /
  `DawToggle` (draw through `Widgets.h:68 DrawButton`), `DawSlider` (a
  `BSlider` overriding `DrawBar`/`DrawThumb`/`DrawHashMarks`/`DrawText`),
  `DawKnob`, `DawTextField` / `DawMenuField` / `DawCheckBox` (draw their own
  frame + label), and `DawControlLook` — a `BPrivate::BControlLook` subclass
  installed process-wide (`be_control_look`, `main.cpp:124`) that draws menus,
  scrollbars, tabs, alerts and every stock control left in a window.
- **`be_control_look` is per-process** (`ControlLook.h:451`): replacing it
  cannot leak into another app. It is initialised once at Interface Kit start
  (`InterfaceDefs.cpp:1496-1504`: a control-look add-on if the user installed
  one, else `new HaikuControlLook()`), so the honest way to get the stock look
  back is to remember the pointer the kit had before we replaced it and put it
  back — never to construct a private class of our own.
- **Kit-free layering** (`CMakeLists.txt:35`): `daw_model` builds and tests on
  this Linux host. `src/app/AppSettings.cpp` is in it; `src/ui/` is Haiku-only
  (checked by `scripts/haiku_syntax_check.sh`, built on the VM).
- **Settings.** `AppSettings` is plain data + a `key value` text round trip
  (kit-free, host-tested in `tests/appsettings_tests.cpp`); the file lives in
  `~/config/settings/HaikuDAW/`.
- **Tests that look at the screen.** `tests/ui_functional_tests.cpp` posts the
  messages the widgets post, installs `DawControlLook` exactly as `main.cpp`
  does, and writes a screenshot per test under `DAW_UI_SHOTS`
  (`docs/UI_GUIDELINES.md` §1-2).

## Goal

Two modes, one code path:

- **System (default)** — the app looks like a Haiku app. Every token derives
  from the user's `ui_color()` values with `tint_color` for the variants;
  stock controls are drawn by the stock look; a change in Appearance is
  followed live.
- **Dark** — today's palette, drawn by `DawControlLook`, in the DAW's own
  windows only.

No `set_ui_color` anywhere in the tree, and switching modes never needs a
restart.

## Work items

1. **Kit-free derivation** — new `src/app/ThemePalette.h/.cpp` in `daw_model`
   (added to `CMakeLists.txt`), STL only, no Haiku headers:
   - `struct ThemeColor { uint8_t r, g, b; }` (the kit-free stand-in for
     `rgb_color`), `Mix(a, b, t)`, `Tint(c, factor)` (Haiku's `tint_color`
     semantics: each channel scaled, clamped — the same maths, host-testable),
     `Luminance(c)` and `ContrastRatio(a, b)` (WCAG relative luminance).
   - `enum class ThemeMode { System, Dark }`.
   - `struct ThemeBase` — the 17 system colours as data (panel, panelText,
     document, documentText, control, controlText, controlBorder,
     menuBackground, menuItemText, menuSelectedBackground,
     menuSelectedItemText, scrollBarThumb, listBackground,
     listSelectedBackground, listItemText, listSelectedItemText,
     keyboardNavigation) — this is what `ui_color()` fills in fit 2.
   - `struct ThemeTokens` — one field per `Col*()` accessor above.
   - `ThemeTokens DeriveTokens(const ThemeBase& base, ThemeMode mode)` — pure.
     Dark returns today's literals exactly (a regression anchor: the dark look
     must not drift while this lands). System derives: `background` = panel,
     `text` = panelText, header/headerHi/chrome/chromeHi from `control` with
     tints, `lane`/`laneAlt` from `listBackground`, `ruler`/`grid` from panel
     tints, `lcd`/`lcdText` from document/documentText (a native recessed well
     is the document colour, not a black hole), `btnOff`/`btnBorder`/`btnText`
     from control/controlBorder, `accent` from keyboardNavigation, `textDim` =
     Mix(text, background, 0.45), and `playhead` = a colour that contrasts with
     `background` (today's near-white on a dark panel; a near-black on a light
     one). The vivid colours that read on both — clip, clipBorder, wave, the
     M/S/R/mon/play lamps, the meter colours and the per-track palette — are
     constants, because a clip is blue in any DAW and white-on-blue stays
     legible.
2. **Theme.h becomes the adapter.** The `Col*()` names and signatures stay;
   each returns `ToRgb(Tokens().field)`. Add: `SetActiveThemeMode(ThemeMode)`,
   `ActiveThemeMode()`, `SetSystemBaseColors(const ThemeBase&)` (fit 3 fills
   it from `ui_color()`), and the refresh entry point (fit 4). `MeterColor`,
   the meter thresholds, `Themed()`, the fonts and `TrackColor` are unchanged.
   Any `Col*()` called before the app installs a base gets the System defaults
   derived from a stock light panel, so a stray call can never return black.
3. **`main.cpp`** — delete `ApplyThemeColors` and every `set_ui_color` call
   (`:129-148`). `ReadyToRun` reads the mode from `AppSettings` (which it
   already loads, through `ProjectDocument`), fills `ThemeBase` from
   `ui_color(...)` when the mode is System, then installs the control look for
   the mode: Dark → `DawControlLook`; System → the pointer the kit already had.
   A `B_COLORS_UPDATED` message (Haiku sends it when Appearance changes) re-reads
   the base, re-derives, and refreshes every window through the fit-4 helper.
4. **Live switching** — one helper in `Theme.h`:
   `ApplyThemeToAllWindows()` (walks `be_app`'s windows and every child view
   depth-first). Views that cache a colour implement a two-line hook; views
   that draw with tokens are just invalidated. Implementors: `MainWindow` (and
   its menu bar), `TransportBar`, `InspectorView`, `TimelineView`, `PianoRoll`,
   `MixerWindow`, `EffectsWindow`, `SampleBrowser`, `PluginBrowser`,
   `InstrumentWindow`, `SendsWindow`, `QuantizeWindow`, `ExportWindow`,
   `ExportProgressWindow`, `RenameWindow`, `Lv2UiWindow`, and the kit controls
   (`AdoptPanelColors` for `DawControl`, the well/label colours for
   `DawTextField` and `DawMenuField`, the box for `DawCheckBox`).
   `AppSettings` gains `themeMode` (kit-free round trip + a host check in
   `appsettings_tests`), and the main window's **View** menu carries a marked
   `Dark Mode` item that flips it (this is where M3.5's Preferences window will
   take it from).
5. **The kit draws through the look** so stock and kit controls agree in both
   modes: `DawButton`/`DawToggle` → `Flags()` +
   `DrawButtonFrame`/`DrawButtonBackground`/`DrawLabel` (a lit toggle keeps its
   lamp colour and auto-contrast label — the stock look cannot express a red
   record lamp); `DawSlider` → `DrawSliderBar`/`DrawSliderThumb`/
   `DrawSliderHashMarks`; `DawCheckBox` → `DrawCheckBox`; `DawTextField` →
   `DrawTextControlBorder` + the well colours; `DawMenuField` →
   `DrawMenuFieldFrame`/`DrawMenuFieldBackground` + `DrawArrowShape`.
   `DawKnob` has no stock equivalent: it stays custom-drawn, from tokens.
6. **The custom surfaces read well on a light panel.** Clip names and headers
   (`ColText` on `ColHeader`), the ruler and grid lines, lane separators, the
   playhead, the inspector's dB/readout rows, the transport readout well, the
   dock's header strip, the piano roll's keys and grid: check the derivations
   against a light panel and fix what does not read. `TrackColor` clips keep a
   contrast-safe label colour (the existing auto-contrast rule).
7. **Tests.**
   - `tests/theme_palette_tests.cpp` (host, new): `ContrastRatio(text,
     background) >= 4.5` in both modes and for a light (`#d6d6d6`-ish) and a
     dark (`#2a2a2a`-ish) system panel colour; the playhead contrasts with the
     background and the grid is visible on it; Dark mode's tokens equal today's
     literals exactly; `Tint`/`Mix` clamp.
   - `appsettings_tests`: `themeMode` round trip, unknown value → default.
   - `tests/ui_functional_tests.cpp`: the mode comes from `DAW_UI_THEME`
     (`system`/`dark`; default system, and the Dark run installs
     `DawControlLook` exactly as the app does), a new check that the View menu
     item flips the mode and that the window's colours actually changed, and a
     `Shot()` per window in whichever mode the run uses — the pass is run
     **twice**, once per mode, and every shot reviewed.
8. **Record** — `docs/agent-prompts/25-theme-modes-PR.md`: what was derived,
   what each mode looks like, the shots reviewed (both modes), the mutation
   check by name, and the note for Marc: system colours already overwritten by
   an earlier run are restored in **Preferences > Appearance > Colors >
   Defaults** (his call; the app does not do it for him).

## Definition of done

- `grep -rn set_ui_color src/` returns nothing.
- Host build exit 0, `ctest --test-dir build-host` green (count in the record),
  ASan green, `scripts/haiku_syntax_check.sh` 0 FAIL, VM `build` and
  `build-off` green, `ui_functional_tests` green and its new checks passing.
- The screenshot pass run in **both** modes on the unlocked VM; every shot of
  every window opened and checked against `docs/UI_GUIDELINES.md` §3, at 150%
  too where geometry moved; what was wrong and fixed named in the record.
- Every new behaviour has a test that was mutation-checked (break it, watch it
  fail, restore), named in the commit body.
- No `Co-Authored-By:` or other AI attribution in commits or PR text.
