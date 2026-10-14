# Record — the mixer and the windows it shares a session with (package 52)

Branch `fix/ux-mixer`, cut from `master` (`0783e5e`). Spec:
`docs/agent-prompts/52-ux-mixer.md`. Eighteen findings, A–R; all eighteen are
addressed, with two partials named at the end.

## Commits

| Commit | Subject |
| --- | --- |
| `84e6984` | `docs(agent-prompts): spec the mixer UX and integrity package` |
| `4e69e67` | `feat(ui): the kit-free rules behind the console's refresh, identity and text` |
| `54fa8c3` | `feat(fx): a live wet/dry setter, and an LV2 enum's own names` |
| `314ddbf` | `fix(ui): the mixer, and the windows it shares a session with (A–R)` |
| `a20de4e` | `docs(agent-prompts): record the mixer UX package` |
| `b62a3c3` | `fix(ui): post the mixer's test keys the way the app_server does` |

## Reopened after the integration run (item P)

The first full VM run reported two failures, both in `TestUxMixer`'s keyboard
checks: the ↑/↓ nudge and the `M` key. **They were the test's fault, not the
mixer's.**

Haiku only routes a key to the focus view when the message is a *focus
message*. `BWindow::task_looper` asks `_DetermineTarget` (which for
`B_KEY_DOWN` returns `CurrentFocus()`) only when the header target is
`B_PREFERRED_TOKEN`, and `BWindow::_HandleKeyDown` bails out on its first line
otherwise (`BWindow::_IsFocusMessage`). A key posted with the **window's own
token** — `window->PostMessage(&key)`, or `BMessenger(window).SendMessage`,
which is what the test did — is not one, so it falls through to
`BWindow::MessageReceived`, which does not handle keys at all, and the event is
dropped. Real events from the app_server carry the preferred token.

The codebase already shows the difference: the FX window's Cmd-Z test works
because `EffectsWindow::DispatchMessage` intercepts that key itself, before the
framework's routing. Where an app does not intercept, only a preferred-token
key reaches `KeyDown` — which is why the mixer's own code was correct and the
test was not.

The keys are now posted to the looper's *preferred handler*
(`BMessenger((const BHandler*)nullptr, (const BLooper*)mixer)`), which is the
faithful shape and exercises the whole real chain: `task_looper` →
`_DetermineTarget` → `CurrentFocus()` → `BView::MessageReceived` → `KeyDown`.

Two diagnostics went with it, because "the keys do nothing" and "the view never
took the focus" are different bugs: the focused view is now printed and
asserted (`CHECK(mixer->CurrentFocus() == strips)` — item P's focusable
requirement, `B_NAVIGABLE` + `MakeFocus`), and the gain/mute lines print
whether the key changed anything rather than printing only on success. A future
run of this check therefore says which half broke.

## Files

New: `src/app/UiSync.h`, `src/model/ParamDisplay.h`, `tests/ui_rules_tests.cpp`,
`docs/agent-prompts/52-ux-mixer.md`.

Changed: `src/dsp/MeterMath.h`, `src/engine/Engine.{h,cpp}`,
`src/plugin/Lv2Host.{h,cpp}`, `src/plugin/Lv2PortMap.h`,
`src/ui/{MixerWindow,MainWindow,EffectsWindow,InspectorView,InstrumentWindow,SendsWindow,TimelineView,TransportController}.{h,cpp}`,
`src/ui/widgets/DawSlider.h`, `tests/{meter_tests,lv2_host_tests,lv2_fixture_tests}.cpp`,
`tests/lv2fixture/{manifest.ttl,plugins.ttl,daw_test_plugins.cpp}`,
`tests/ui_functional_tests.cpp`, `CMakeLists.txt`, `docs/USER_GUIDE.md`.

## Verification (this tree, at `b62a3c3`)

1. `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2`

```
configure exit=0
build exit=0
[100%] Linking CXX executable projectio_fuzz_tests
[100%] Built target projectio_fuzz_tests
```

2. `ctest --test-dir build-host`

```sh
76/76 Test #76: projectio_fuzz_tests .............   Passed    0.47 sec

100% tests passed out of 76

Total Test time (real) =   2.76 sec
```
(Re-run after the item-P test fix at `b62a3c3`: build exit 0, **100% tests
passed out of 76**, 3.53 s. The integration branch carries its own extra host
tests, so its count is higher; this branch's is 76.)

(The 76 includes the new `ui_rules_tests` and the two extra checks files below.
Green before and after the package, so nothing regressed.)

3. `sh scripts/haiku_syntax_check.sh`

```
exit=1
OK: 83  FAIL: 4
FAIL src/engine/FlacSink.cpp
FAIL src/engine/FlacSource.cpp
FAIL src/engine/VorbisSink.cpp
FAIL src/engine/VorbisSource.cpp
```

The four failures are the pre-existing FLAC/Vorbis ones the brief names — no new
failures. Two changed files are NOT covered by this check on the host and were
verified by review plus the fixture suite instead: `tests/lv2fixture/*.ttl` (read
by lilv at test time) and the CMake wiring.

New check counts: `ui_rules_tests` 60 checks, `meter_tests` 131 (was ~100),
`lv2_fixture_tests` 117 (was ~107), `lv2_host_tests` 330.

## Mutations (each applied to the tree, run, and reverted)

| # | Mutation | Caught by |
| --- | --- | --- |
| M1 | `GainToFaderFrac` made linear in gain again | `meter_tests` — 8 failures (the round trip and the mid-travel dB checks) |
| M2 | `FaderUnityFrac` returns a fixed 0.75 | `meter_tests` — the unity-fraction check |
| M3 | `NudgeGainDb` without the top clamp | `meter_tests` — "clamped at the top" |
| M4 | `ProjectScope::Stale` always false | `ui_rules_tests` — 4 failures in the scope checks |
| M5 | `TrackLayoutDrift` compares the ids as a SET | `ui_rules_tests` — the Reordered check |
| M6 | `TrackLayoutDrift` reports the addition before the removal | `ui_rules_tests` — the both-at-once check |
| M7 | `ParamUnitFromName` takes the FIRST bracketed group | `ui_rules_tests` — 8 failures ("(Hz)", "Gain (L/R) (dB)") |
| M8 | `ScalePointIndex` requires exact equality | `ui_rules_tests` — the 2.0001 tolerance check |
| M9 | `ParamValueText` appends the unit to a scale-point name | `ui_rules_tests` — 3 failures |
| M10 | `Lv2Host` stores empty scale-point labels | `lv2_fixture_tests` — 3 failures (the fixture enum's names) |
| M11 | `TrackLayoutOf` returns an empty layout | `ui_rules_tests` — 11 failures |

Mutations for the Haiku-only behaviour (the window close on a project
replacement, the mixer's keyboard, the pinned master, the undoable master
fader) are not runnable here; each is asserted by `TestUxMixer` on the VM, and
the mutation for those is the same edit the finding describes (revert the call
and watch the check fail). Called out rather than blended in: **the Haiku-only
half of this package is unverified by me** — I could not run the VM (the brief
forbids it), so the functional suite and the screenshot pass are the
orchestrator's.

## What to look for in the screenshots (both theme modes)

`Shot("ux-mixer")` is taken with the mixer in front:

- The **master strip is at the right edge of the window**, whole, with its
  fader, its two meter bars, its lamp and its value row — not scrolled off and
  not overlapped by a track strip;
- strips are one per track in the arrangement's order, each with `In`,
  insert rows, `Snd`, `→Mst`/`→Bus`, `Auto`, `Inst` (MIDI only) — no button
  drawn as disabled-looking that is actually live, and `Auto` lit only when a
  lane mode is set;
- each strip's meter: **two narrow bars** (L/R) with a white hold tick and a
  small lamp above them; nothing overlapping the dB ticks beside them;
- the **name bar** at the strip's foot: the long track name ellipsised inside
  its own bar, not printing over the next strip;
- a **horizontal scrollbar along the bottom** of the rack, its thumb sized to
  the visible fraction, and the rack scrolled by it;
- in Dark and System mode: no near-black rectangle on a light panel (the
  insert rows, the wells, the meters and the lamps are all tokens now).

Elsewhere:
- `Shot("fx-panel-sidechain")` and the LV2 parameter panel shots: the value
  column reads names ("Lowpass") and units ("440 Hz") where the plugin declares
  them, and the EQ graph shows a band readout while a handle is hovered;
- `Shot("widget-kit-used")` / the Sends window: each level row prints its level
  in dB above the bar;
- the Instrument window: the ADSR rows print ms/s;
- `Shot("meter-playing")` / `Shot("meter-clipped")`: the header meters' lamps.

## Findings that were wrong or partial when I reached the code

- **G is partial.** The master volume slider in the transport bar cannot show
  its value: a `BSlider` with value text reserves a text line ABOVE its bar
  (Haiku's `BarFrame` moves the bar down by a font height), and that row is
  20 px in a 54 px bar whose rows above it are full — the text would push the
  trough and its handle out of the view. Widening the bar's rows is the
  transport bar's own layout (M3.1/M3.3), not this package's, and I could not
  look at the screen to check a guess. The sends' level rows and the ADSR rows
  DO show numbers, and the mixer's master strip already showed dB. The slider
  was left exactly as it was, with a comment saying why.
- **F is partial, as the brief allows.** Per-track RMS does not exist in the
  engine (only the master runs the sliding `StereoMeter`), so track strips and
  the header/inspector meters have no RMS line. The master strip draws RMS
  (it now travels with the peaks payload) and every meter has the hold tick,
  the stereo split and the clip lamp.
- **C's "Read"** is renamed **Auto**. There is no read on/off state in this
  app — automation is always read — so a "Read" lamp could not be lit by
  anything real. The button cycles the track's automation-lane mode (the
  timeline's own Auto box) and its lamp follows that mode, which is the
  behaviour the finding's parenthetical asked for.
- **A's ordering.** The brief says `ShowReplacedProject` is the natural home,
  and it is where the close lives. One detail the finding did not mention: the
  effects panel flushes a pending wheel edit when it quits, and a commit
  arriving after the load would name a track id that now means another track.
  The panel is told to drop that edit (kMsgFxDropPending) before the close
  request, so the close cannot post a stale chain into the new session.
- **B's net is only live while the pulse runs.** `PushTrackPeaks` re-sends a
  drifted layout, and the pulse only runs while playing/monitoring/recording
  (or during a Stop's meter decay). The four announced posts are what covers
  the idle case; the net is the belt under them, not a replacement. Said here
  because a reader could otherwise assume the rack is self-healing while
  stopped.
- `MainWindow::ShowInstrumentWindow` now refuses a non-MIDI track: its button
  is only drawn for MIDI tracks, and a window opened on a stale snapshot for an
  audio track would edit a voice that track does not have.

## Not verified here

- The VM: `ui_functional_tests` (including the new `TestUxMixer`) and the
  `DAW_UI_SHOTS` pass in both theme modes. Nothing in this package has been
  looked at on a screen; by `docs/UI_GUIDELINES.md` §1 that means the UI half
  is NOT done until someone opens those shots.
- `Engine::SetFxMixLive`, `MixerWindow`, `EffectsWindow`, `DawSlider` and the
  rest of `src/ui/` do not link on this host; they are compile-checked
  (`haiku_syntax_check.sh`) and reviewed, not exercised.
- The mixer's fader law and the strip geometry were reasoned about from
  Haiku's own `Slider.cpp`/`ScrollBar.cpp` sources; the 20 px master-slider row
  above is the one place that reading changed a decision.
