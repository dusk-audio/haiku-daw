# Task: Theme (plan M1.2) — slice B, scaled metrics

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/theme-scale` off master (M0 and M1.1 complete). M1.2 has two halves:
the tokens (slice A, merged) and making every shared layout metric a function of
the user's font size (slice B, this task). The release checklist has a "visual
pass of every window at 100% and 150% font size" item; today only the tokens
exist, so a 150% font would draw 150% text inside 100% boxes.

## What slice A left (merged 2026-10-09, `564422a`)

`src/ui/Theme.h` holds the colour tokens, the type tokens (`ThemeFontSize()`,
`ThemeFont()`, `ThemeFontBold()`), the scale (`kDesignFontSize = 12.0f`,
`ThemeScale()`, `Themed(v)`) and ONE meter threshold set (`kMeterWarn` /
`kMeterClip` / `MeterColor`), which ended a real 1.0/0.7-vs-0.9/0.6
disagreement between `MeterView` and `Theme.h`. `UiMetrics.h` includes it, so
`#include "UiMetrics.h"` still reaches every token. **Nothing uses `Themed()`**
except the declaration itself: `UiMetrics.h`'s constants are still plain
`constexpr float`, so the scale is declared and demonstrated but not applied.

## Slice B — every shared metric scales

1. **`UiMetrics.h`**: the six layout constants become inline functions over
   `Themed()`, with the design values kept beside them as `kDesign*`:

   ```cpp
   constexpr float kDesignRulerHeight = 28.0f;
   inline float RulerHeight() { return Themed(kDesignRulerHeight); }
   ```

   `RulerHeight`, `TrackHeight`, `TrackGap`, `HeaderWidth`, `HdrMeterW`,
   `InspectorWidth`. Delete the old constant names so the compiler finds every
   site — a stale unscaled `kTrackHeight` left behind is exactly the bug this
   slice exists to prevent.

2. **The arrangement view's rects** (`TimelineView.cpp`): the header control
   rects (`MuteRect`/`SoloRect`/`ArmRect`/`MonRect`/`PanKnobRect`/`GainRect`)
   are design metrics — scale every offset in them, and the lane-height clamp
   (`LaneHeightOf`'s 300). Drawing and hit-testing both go through these
   helpers, so the click targets follow for free — that is the point of doing
   them first.

   `Track::height` is a MODEL value in pixels, serialized in the project. Leave
   the per-track height itself alone (M2.6 owns lane-height dragging); the
   clamped default lane height is what scales.

3. **`Widgets.h`**: the shared drawn controls (knob, rounded button, fader,
   VU meter) carry design-pixel geometry — knob radius inset and ring width,
   corner radii, the fader trough width and 24x12 cap, text baselines. Scale
   them. These are what the per-window panels will draw with, so scaling here
   scales every panel that uses them.

4. **`MainWindow.cpp`**: the inspector/timeline split (`kInspectorWidth`) and
   the transport bar height (36) scale with the rest.

5. **A test hook for the scale.** `ThemeScale()` gains an override
   (`SetThemeScaleOverride(float)`, 0 = derive from the font) so
   `ui_functional_tests` can exercise a 150% layout on the VM, where the font
   size itself cannot be changed mid-run. The font tokens stay the real font:
   the override exists to prove the METRICS scale and that layout and
   hit-testing agree at a non-100% scale.

## Explicitly NOT in this slice

The per-window constants (`MixerWindow.cpp`'s strip width, `PianoRoll.cpp`'s
keyboard column and row height, `EffectsWindow.cpp`'s dial geometry, the
dialog paddings). M1.4 rebuilds those windows with the Layout Kit and the M1.3
widget kit; scaling them now means scaling them twice. The record states the
boundary so it reads as a decision, not an oversight.

## Definition of done

- `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- VM `build` and `build-off` ctest green; `ui_functional_tests` grows by a
  deliberate check: at override 1.5 the metrics are 1.5x, and a synthetic
  click at the scaled lane geometry selects the track it lands on (layout and
  hit-testing agree). Mutation-checked: revert one `Themed()` call to the raw
  design value and the check fails.
- Host suite unchanged (this is Haiku-only code) — counts in the record.
- PR record `docs/agent-prompts/20-theme-PR.md`; the status table in
  `docs/agent-prompts/README.md` gets the slice.
