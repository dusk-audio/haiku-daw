# Task: the mixer and the windows it shares a session with — UX and integrity (18 findings)

A UX review of the mixer, the effects editor and the two windows the strips
open produced a ranked findings list (A–R). The first five are integrity bugs —
a stale panel that writes into the wrong project, a console that does not
follow the model, three buttons drawn but not wired, a master fader outside the
undo stack, a window that does not fit the screen. The rest are feel: meters,
keyboard, drag laws, numbers on controls, colours.

Branch: `fix/ux-mixer`, cut from `master`.

## What the review found, and what this package does about it

### A (HIGH, integrity) — a window open across File ▸ Open keeps editing the NEW project

`MainWindow::LoadFrom` flushed and closed only the LV2 *native* editors
(`CloseFxEditors`, the same rule `ShowReplacedProject`'s doc comment states).
The generic effects window (`fFxMsgr`), the mixer (`fMixerMsgr`) and every
ad-hoc Sends/Instrument window were told nothing, and `ShowReplacedProject`
reset only the inspector, the dock, the record arms and the master slider.
Track ids **restart at 1** in a loaded file (`LoadFrom` says so), so a strip
click or a knob move on a panel from the old session lands on the new project's
same-numbered track — and the effects window commits its **whole chain**, which
is a data-corrupting write saved as an undoable edit.

**Fix.** Every window whose content is addressed by track id is closed when the
project is replaced, at the one place every replacement path goes through
(`ShowReplacedProject`): the mixer, the effects editor, the sends editor, the
instrument editor. The native editors keep their own `CloseFxEditors`.

The rule is extracted, kit-free and host-tested — `src/app/UiSync.h`:

- `class ProjectScope` — the generation counter a project replacement bumps. A
  window records the generation it opened against; `Stale(seen)` is the whole
  predicate. The point is that the *rule* is one comparison, not a list of
  window types somebody has to remember to extend (which is how the bug got
  in).
- `TrackLayoutOf(const Project&)` / `TrackLayoutMatches(shown, model)` — the
  model's track ids in order, and "is the console showing the model's layout?".

### B (HIGH) — the mixer does not follow the model

The rack is refreshed only from the `kMsgUiRefresh` funnel, which four
mixer/marker-originated sites posted. Track ▸ New/MIDI/Bus, Track ▸ Duplicate,
and the timeline header's delete and reorder did not: a new track had no strip,
a deleted track left a live-looking strip whose every edit the `FindTrack`
guard silently dropped, and a reorder left the console in a different order
from the arrangement.

**Fix, two layers.**

1. The four edit sites post `kMsgUiRefresh` (one line each, the pattern that
   already existed at the other sites).
2. The refresh is funnelled through one `MainWindow::PushMixerStrips()`, which
   records the layout it sent; `PushTrackPeaks` — the tick that already runs at
   60 Hz while the transport does — compares that record with the model's via
   `TrackLayoutMatches` and re-sends when it has drifted. A structural edit
   that nobody remembered to announce corrects itself within a tick.

### C (HIGH) — three buttons on every strip do nothing

`In`, `→<Bus>` and `Read` were painted as buttons, lit by state, and
`MouseDown` had no case for them, so the click fell through to "select track".
`UI_GUIDELINES.md` §3: "Disabled controls look disabled."

**Fix.** All three are wired, through messages the main window answers — the
mixer's snapshot holds labels, not model structures, so intents travel:

| Strip control | Message | What the main window does |
| --- | --- | --- |
| `In` | `kMsgMixInput` (+ the click's screen point) | opens the inspector's input picker for that track (`InspectorView::PickInput` — the same code the inspector's own button runs, one path not two) |
| `→Mst`/`→<Bus>` | `kMsgMixOutput` | the inspector's output picker (`InspectorView::PickOutput`) |
| `Auto` (was `Read`) | `kMsgMixAuto` | `TimelineView::CycleAuto(track)`, then a refresh |

`Read` is renamed **`Auto`**: the strip has no read on/off state to show — the
engine always reads automation — and the button now drives the one automation
mode a track has (the timeline's lane mode), lit while that mode is not Off.
The snapshot carries `autoMode` (payload field `am`) so the lamp is real.

### D (HIGH, data loss) — the mixer's master fader bypassed the undo stack

`ApplyStrip` posted `kMsgApplyMaster` per mouse-move and the handler wrote
`fProject->masterGain` directly, while the transport bar's own master slider
went through `SetMasterGainCommand` with a comment saying why. No Cmd-Z for a
master move, and — the dirty flag derives from command serials — a session
whose only change was the master level reported **clean**: File ▸ Close gave no
prompt and the level was lost.

**Fix.** `kMsgApplyMaster` runs `SetMasterGainCommand` (it coalesces a drag
into one undo step) and calls `UpdateTitle()`.

### E (HIGH) — the window does not fit the screen; the master is off the right edge

The frame was computed from the strip count and never clamped, no `BScreen`
call existed in the mixer, and the master strip was drawn at
`StripX(fStrips.size())` — the far end of the content — so on a 1024-wide
screen the master fader and meter were off-window with nothing saying so.
Wider-than-view content was reachable only by the wheel, and the window is
`B_NOT_ZOOMABLE`.

**Fix.**

- The initial frame is clamped to `BScreen(this).Frame()` (the pattern
  `EffectsWindow::FitToContent` uses) with the strip's own minimum height kept.
- The rack gets a real horizontal `BScrollBar` (a `BScrollView` around the
  strips view), so the console is scrollable by pointing at it, and the
  scrollbar is kept in step with the wheel pan and with a strip-count change.
- **The master strip is pinned to the view's right edge, outside the scrolled
  content** — a console's master is always in front of you. It no longer
  travels with `fScrollX`, and the scroll range excludes it.
- The effects editor's "Add Effect…" browser (`f.right + 12`, no clamp) is
  clamped to the screen the same way.

### F — meters

`DrawMeterBar(this, L.meter, std::max(peakL, peakR), false)` — one bar for two
channels, no hold, no lamp. The helper supports `rms`/`holdFrac` and the mixer
already receives both channels.

**Fix.** Mixer strips: two bars (L/R), each with `PeakHold` and a `ClipLatch`
that latches at `ReachesClipLevel` and clears when the lamp is clicked; the
master strip also draws the RMS line (its RMS now travels with the peaks
payload: `mrl`/`mrr`). Per-*track* RMS does not exist in the engine (only the
master runs the sliding `StereoMeter`), so track strips have no RMS line — said
here rather than faked. Timeline header meters and the inspector's meter get
the same hold + lamp treatment (per-track RMS is unavailable for the same
reason).

### G — no control shows its value as a number

`DawSlider::DrawText` returns early on empty text and no call site supplied
one.

**Fix.** `DawSlider` gains an optional value formatter
(`SetValueFormatter(std::function<std::string(int32)>)`, with `UpdateText()`
overridden to serve it). Set on: the sends' level rows (dB), the master volume
slider (dB), and the instrument's ADSR rows (ms below a second, s above).

### H — the effects editor lacks typed-entry niceties

Knobs were a bare relative drag, parameter sliders mapped absolutely, Shift
worked only in `WheelAdjust`, and double-click reset existed for exactly two
control kinds (9 and 12) — not for the built-in knobs (0) or the EQ band
handles (5).

**Fix.** Shift divides the drag rate by 10 on both drag paths (knob and
parameter slider, and the EQ handle's two axes). Double-click resets kind 0 to
the descriptor's own default (`MakeDefault(type).p(slot)`) and kind 5 to that
band's default from `EqDesc()` — both through `EnsureParamSlot`, so a
descriptor that is short of the slot grows to the default value rather than to
zero.

### I — the EQ graph shows no numbers for the band being edited

**Fix.** The active band's frequency, gain and Q are printed in the graph while
its handle is hovered or dragged. Hover is tracked in `MouseMoved` (which
previously did nothing outside a drag), and the readout is drawn inside
`DrawEqGraph` so the number is where the eye already is.

Q was reachable only by hovering the handle with the wheel. That stays the
gesture — the readout is what was missing — and the wheel's Q step now also
respects the Shift fine rate (H).

### J — LV2 parameters with scale points print raw floats

A filter type showed "2" where the plugin says "Lowpass", a toggle showed 0/1,
nothing carried a unit.

**Fix.** `Lv2ParamInfo` gains `scalePointNames` (parallel to `scalePoints`,
read from `lilv_scale_point_get_label`), and `HostParam` carries it. The
editor draws the scale point's name when the value lands on one, else the
number, and appends the unit when the port's *name* declares one —
`Frequency (Hz)`. That extraction is kit-free and host-tested
(`src/model/ParamDisplay.h`: `ParamUnitFromName`, `ScalePointIndex`,
`FormatParamValue`).

### K — the wet/dry slider did not sound until release

Every other parameter row in the same panel previews live. The engine models
per-insert mix as an atomic array (`Graph::Bus::fxMix` / `fMasterFxMix`) that
the RT thread reads each block, and `SyncFx` pushes into it — there was simply
no live setter.

**Fix.** `Engine::SetFxMixLive(track, master, fxIndex, mix)` stores into that
same atomic (the operation `SyncFx` already performs, addressed from the UI),
and the mixer-style preview path is reused: `kMsgFxLiveMix` during the drag,
the ordinary `kMsgApplyFx` commit on mouse-up. No engine rebuild, no seam.

### L — Sends and Instrument windows are not single-instance

Each ask built a new window, and each window holds a **whole-snapshot** copy
which it posts back, so two windows of one editor silently overwrote each
other's edits — the last one to touch a control won, and the intervening edit
was gone.

**Fix.** One messenger per kind, carrying the track it is pointed at, exactly
the shape the effects window has had since T2:

- a second ask for the same track **activates** the open window;
- an ask for another track **retargets** it (new snapshot, new title naming the
  track) instead of stacking a second one;
- when its track is gone, the window is **closed** — checked in the
  `kMsgUiRefresh` funnel, which every track-list edit posts (B). The effects
  editor gets the same close-on-track-deleted check, since a chain snapshot
  addressed by a dead track id is the same hazard.

### M — long track names print over the next strip

The mixer's name bar drew the name untruncated, in a view that truncates
everything else it draws; the effects panel's title had the same bug with the
`Del`/`^`/`v` buttons drawn over the same row.

**Fix.** `TruncateString(B_TRUNCATE_END)` into the name bar's own rect, and the
effects title truncates to the room the title row's buttons leave — the width
its own `Hit` rects already reserve.

### N — meters and the loudness readout freeze at their last playing value on Stop

The 60 Hz pulse only runs while playing, and `StopPlayback` reset the main
meter but not the mixer's peaks.

**Fix.** `StopPlayback` pushes zeros to the mixer (per track and master) and
opens a short *decay window* — `UpdatePulse` keeps the pulse alive until it
closes, so the holds fall at `PeakHold`'s own rate instead of freezing at the
last playing value, and the master meter's hold decays the same way. Clip lamps
deliberately stay latched (a lamp that cleared itself when the transport
stopped would hide the clip that just happened).

### O — hard-coded colours in the mixer and the effects wells

`Rgb(16,18,22)`, `Rgb(12,14,17)`, `Rgb(46,96,86)`, `Rgb(232,150,70)`,
`Rgb(120,60,60)`, `Rgb(150,110,50)`, `Rgb(52,104,74)`, `Rgb(70,90,130)`,
`Rgb(200,165,90)` — all read as near-black rectangles inside light panels in
System mode, which is exactly what `UI_GUIDELINES.md` §5 forbids.

**Fix.** Every one is a token: wells and graph backgrounds `ColLcd()`, grid and
outlines `ColGrid()`, lamps `ColRec()`/`ColMon()`/`ColSolo()`/
`ColAudioAccent()`/`ColMidiAccent()`, the master's wood tone
`MixColor(ColAccent(), ColTextDim(), …)`, the spectrum fill `ColPlay()` mixed
toward the background.

### P — the mixer cannot be driven from the keyboard at all

**Fix.** `MixerStripsView` is focusable (`B_NAVIGABLE`), keeps a *current
strip* (the one `kMsgMixSelect` already names, drawn with a focus ring), takes
focus on a click, and handles: Left/Right to move the current strip,
Up/Down to nudge its gain by 0.5 dB, `M`/`S`/`R` for mute/solo/record-arm.
The master strip is reachable as the last position (Up/Down move its gain).

### Q — the fader law is linear in gain with a +3.5 dB ceiling

A typed level above +3.5 dB was silently clamped, while the field advertises
`[-96, +12]` via `ParseDecibels`.

**Fix.** The fader is mapped through **dB** over exactly the range the field
advertises (`kFaderMinDb`..`kFaderMaxDb`, both in `src/dsp/MeterMath.h` where
the typed-level clamp already lived): `FaderFracToGain` / `GainToFaderFrac` /
`FaderUnityFrac`, kit-free and host-tested. Zero dB now sits at 8/9 of the
travel, the bottom of the travel is true silence (`GainFromDb(-96) == 0`), and
there is no second, silent clamp anywhere: what the field reads back is what
the model holds.

### R — drags jump absolutely on click, and the wheel pans instead of adjusting

**Fix.** The mixer's fader drag is grab-relative (the effects view's own law)
with Shift for a fine rate; the trim/pan/send knobs already were. The wheel
adjusts the control under the pointer (fader, pan, trim, a send knob, or a
fader's value) and pans the rack only from bare panel space.

## Kit-free extraction (host-tested, `ctest --test-dir build-host`)

| Rule | Header | Test |
| --- | --- | --- |
| the fader law (Q) | `src/dsp/MeterMath.h` | `tests/meter_tests.cpp` |
| the stale-window identity rule + the strip-payload refresh rule (A, B) | `src/app/UiSync.h` | `tests/ui_rules_tests.cpp` |
| parameter text: unit from the port name, scale-point names (J) | `src/model/ParamDisplay.h` | `tests/ui_rules_tests.cpp` |

## Verification

- `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2` (exit code checked)
- `ctest --test-dir build-host`
- `sh scripts/haiku_syntax_check.sh` — no NEW failures
- `tests/ui_functional_tests.cpp` gains a `TestUxMixer` section: the project
  replacement closes the track-addressed windows (A); a track added, deleted
  and reordered reaches an open rack's strips (B); the master fader is an
  undoable, dirtying edit (D); a strip keyboard cycle changes the gain and the
  toggles through the model (P); the fader law's own numbers (Q) — plus a
  `Shot("ux-mixer")`.
- On the VM (the orchestrator runs it): the `DAW_UI_SHOTS` pass in both theme
  modes. What to look for, shot by shot, is in `52-ux-mixer-PR.md`.

## Not in this package

- Per-track RMS on the strips and the header meters: the engine meters stereo
  RMS on the master only. Not faked, not plumbed.
- A "Read" automation *mode* button on the strip: there is no read on/off state
  in this app — automation is always read. The button drives the lane mode
  instead (see C).
