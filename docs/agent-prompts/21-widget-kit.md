# Task: Dark widget kit (plan M1.3)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/widget-kit` off master (M0, M1.1 and M1.2 slice B complete). The app
draws its big surfaces itself (timeline, inspector, mixer strips, piano roll)
but every DIALOG is stock Haiku: light `BButton`s, `BTextControl`s and
`BSlider`s on a dark panel. That mismatch is the "prototype" tell the plan's
audit named.

## What exists

- `src/ui/Theme.h` — colour, type and the scale; `Themed(v)` is how a design
  pixel becomes a device pixel (M1.2).
- `src/ui/Widgets.h` — inline DRAWING helpers (`DrawKnob`, `DrawButton`,
  `DrawFader`, `DrawVUMeter`) used by the custom surfaces. They are not
  controls: no hover, no focus, no messages.
- `src/ui/EffectsWindow.cpp` has the app's one real knob-control, hand-rolled
  inside that window.

## The kit (`src/ui/widgets/`)

Every control is a `BControl` (or the nearest stock subclass) that draws itself
with the theme, and every VALUE control gets the same five behaviours:

- hover, pressed and focus states (focus ring in `ColAccent()`);
- double-click resets to the default value;
- Shift = fine adjustment (1/10 rate);
- the wheel changes the value;
- a `SetToolTip()` on every control (a tooltip with the parameter name).

Files:

| File | Provides |
|---|---|
| `DawControl.h` | the shared base: hover tracking, focus ring, fine/wheel/reset plumbing, `Themed()` geometry |
| `DawButton.{h,cpp}` | `DawButton` (push), `DawToggle` (momentary latch: M/S/R/arm) |
| `DawSlider.{h,cpp}` | `DawSlider` (linear, horizontal/vertical) and `DawFader` (channel fader with a unity tick) |
| `DawKnob.{h,cpp}` | the rotary (lifted from `EffectsWindow`), bipolar or unipolar, with a value ring |
| `DawTextField.{h,cpp}` | `BTextControl` subclass: dark field, caret + selection colours, commit on Enter/focus-out |
| `DawMenuField.{h,cpp}` | `BMenuField` subclass: dark label + arrow |
| `DawCheckBox.{h,cpp}` | `BCheckBox` subclass (also `DawRadioButton`) |
| `DawListView.{h,cpp}` | `BListView` subclass: dark rows, themed selection and scrollbar |
| `DawProgress.{h,cpp}` | `BStatusBar` subclass |
| `DawAlert.{h,cpp}` | `BAlert` subclass: dark panel, themed buttons (see M0.4's `ReportError`) |
| `DawControlLook.{h,cpp}` | a `BControlLook` subclass so any stock control left over (menus, scrollbars, tabs) matches |

Install the look once, in `DawApplication::ReadyToRun`:
`be_control_look = new DawControlLook();` — after that, a stock `BMenu` draws
dark without touching its code.

## Rules

- **Haiku-only.** `src/ui/widgets/` cannot compile on the Linux host; the
  checks are `scripts/haiku_syntax_check.sh` (0 FAIL) and the VM suites.
- **A new .cpp must join `daw_ui`'s source list in `CMakeLists.txt`** (M1.1's
  lesson: the syntax check compiles one file standalone, so only the LINK
  catches a missing entry).
- Geometry is design pixels through `Themed()` — the kit must be correct at
  150% from its first commit, and `ui_functional_tests` runs a 150% pass
  (`SetThemeScaleOverride`, added in M1.2 slice B).
- Controls keep their stock message protocol (`B_CONTROL` / `Invoke`), so
  converting a window is swapping the class, not rewriting the handlers.
- No behaviour changes beyond the five listed behaviours: a converted window
  must answer the same messages with the same values. `ui_functional_tests`
  drives these windows by posted message; those checks are the contract.

## Conversion order (one slice/commit each, each VM-verified)

1. `RenameWindow` (a text field + a button — proves the kit end to end),
2. `QuantizeWindow`, `SendsWindow`,
3. `SampleBrowser`, `PluginBrowser`,
4. `InstrumentWindow`, `ExportWindow`, `ExportProgressWindow`,
5. `TransportBar` internals (its Play/Stop/Rec buttons and the LCD), and the
   `MainWindow` transport-bar controls (BPM field, master slider).

`MixerWindow`, `PianoRoll`, `TimelineView` and `InspectorView` are
custom-drawn; they consume `Widgets.h` and are restyled by M1.4/M2, not here.
`EffectsWindow`'s knob is replaced by `DawKnob` when that window is rebuilt in
M1.4 — until then leave it alone so the two do not drift in one commit.

## Definition of done (kit slice)

- The kit builds on the VM, `haiku_syntax_check.sh` 0 FAIL.
- `ui_functional_tests` green at the same check count (a conversion adds no
  behaviour); any NEW behaviour (hover/press visuals) that cannot be asserted
  programmatically goes on the PR's click list for Marc.
- Host suite untouched (Haiku-only code); counts in the record.
- PR record `docs/agent-prompts/21-widget-kit-PR.md`; the status table in
  `docs/agent-prompts/README.md` gets each converted window as it lands.
