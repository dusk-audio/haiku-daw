# M1.2 theme — PR record

Branch `feature/theme-scale` off `master` (`100e535`), merged `--no-ff`.
Spec: `20-theme.md`. Slice A (`564422a`, the tokens) had landed on master before
this branch; this record covers slice B.

## What slice A left

`Theme.h` had the colour tokens, the type tokens, `kDesignFontSize`,
`ThemeScale()`, `Themed()` and the one meter threshold set — and **nothing
called `Themed()`**. The scale was declared and demonstrated, nothing more: a
150% font drew 150% text inside 100% boxes.

## Slice B — the shared metrics are accessors now

| File | Change |
|---|---|
| `UiMetrics.h` | the six layout constants become `RulerHeight()`, `TrackHeight()`, `TrackGap()`, `HeaderWidth()`, `HdrMeterW()`, `InspectorWidth()`; the design values stay beside them as `kDesign*`. The old names are deleted, so the compiler had to find all 80 call sites |
| `TimelineView.cpp/.h` | every use goes through the accessors; the header control rects (`MuteRect`…`GainRect`), lane insets, ruler ticks/labels/markers, clip blocks and their name strips, fade grips, grab bands, automation handles and the lane-height clamp (300) are `Themed()` |
| `Widgets.h` | the shared drawn controls (knob, rounded button, fader) scale their geometry |
| `MainWindow.cpp` | inspector/timeline split, the transport bar's height and children, and `LayoutTransportBar`'s pinned design offsets |
| `Theme.h` | `SetThemeScaleOverride()` — a test hook, 0 = derive from the font |

Design decision inside `TimelineView`: drawing and hit-testing share the same
rect helpers, so making the helpers scale-aware moves the click targets with the
drawing. That is why the header rects were done first.

## What is deliberately NOT scaled

- **Per-window constants** — `MixerWindow.cpp` (strip width, fx row height),
  `PianoRoll.cpp` (keyboard column, row height, toolbar), `EffectsWindow.cpp`
  (dial geometry, panel paddings), `TransportBar.cpp` (its button rects), the
  dialogs' paddings. M1.4 rebuilds these with the Layout Kit; scaling them now
  means scaling them twice.
- **`InspectorView`** — its own absolute geometry, rebuilt as a split-view pane
  in M1.4. At 150% today the inspector column widens but its rows keep their
  design pitch: the known rough edge until M1.4.
- **`Track::height`** — a MODEL value in pixels, serialized in the project
  (M2.6 owns lane-height dragging). Only the clamped default lane height scales.

## Verification

- Host suite `build-host`: **52/52** (unchanged — this is Haiku-only code).
- `scripts/haiku_syntax_check.sh`: **10 OK, 0 FAIL**.
- VM `build`: ctest **54/54**, `ui_functional_tests` **155 checks, 0 failures**
  (was 141 — this slice adds 14 deliberate checks).
- VM `build-off` (`-DDAW_LV2=OFF`): ctest **50/50**.
- **Mutation check**: `RulerHeight()` reverted to the raw design value →
  `ui_functional_tests` fails twice (the metric value, and the ruler-seek
  behaviour at 150%). Reverted.

## The new check, and what writing it found

`TestThemeScale` forces the scale to 1.5, asserts the six metrics scale, and
then drives the VIEW: a click 7 design-pixels below the DESIGN ruler bottom
(y = 35) must seek at 150% (35 < 42) and must NOT seek at 100% (35 is lane
content there). One of the two answers flips if any part of the chain keeps a
fixed 28. Two header clicks at the scaled lane geometry then assert the selected
track, i.e. that the drawn lane and the click target are one computation.

Writing that check surfaced a **harness bug**: a mouse message posted straight to
a view arrives at `MouseDown` with `where` = (0,0) unless it also carries
`screen_where` — the window derives the view-relative point it hands the handler
from the screen point. The older click check only asserted focus, and focus is
taken before any coordinate is read, so it had never noticed. Synthetic clicks
now carry both points; the comment in the test says why.

## Follow-ups this slice creates

- M1.4 scales/replaces the per-window geometry listed above; until then, a 150%
  font is correct in the arrangement view and the shared widgets only.
- `M1.3` (dark widget kit) inherits the contract: kit controls are
  `Themed()`-correct from their first commit — spec in `21-widget-kit.md`.
