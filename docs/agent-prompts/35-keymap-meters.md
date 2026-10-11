# Task: one keyboard map, and meters that say something (M3.2 + M3.3)

You are working in the Haiku DAW repo (C++17/CMake, Haiku OS, owner Marc). Work
on branch `feature/keymap-meters` off `master`. This is plan items **M3.2
(keyboard map)** and **M3.3 (meters)** of `docs/PLAN_1.0.md` — nothing else in
M3 (the transport bar, mixer strips, preferences, project settings and
templates are other packages).

Read first: `docs/HANDOFF.md`, `docs/UI_GUIDELINES.md` (this touches
`src/ui/`), `docs/PLAN_1.0.md` §M3.2–3.3, `docs/agent-prompts/24-continue-1.0-ENTRY.md`
§5 T6, §6 and §7, and this file.

## Codebase orientation (read before coding)

- **Kit-free code builds and tests on the Linux host**
  (`cmake -S . -B build-host && cmake --build build-host -j8 && ctest --test-dir build-host`).
  `src/model/` and `src/dsp/` are kit-free (STL only); `src/ui/` and
  `src/engine/Engine.cpp` are Haiku-only — pattern-faithful edits, checked with
  `sh scripts/haiku_syntax_check.sh` and built on the VM (`sh scripts/vm.sh build`).
- **Keys.** `MainWindow::DispatchMessage` (`src/ui/MainWindow.cpp:587`) hands a
  key with no Cmd modifier to `fTimeline->MessageReceived` whenever a text field
  is not editing; `TimelineView::KeyDown` (`src/ui/TimelineView.cpp:250`) is the
  switch that currently owns the arrange-window keys (arrows, `l`, Home, +/−,
  `f`, space, `m`, PgUp/PgDn, Del, Ctrl-D, Esc) and posts
  `kMsgTransportToggle` (`src/ui/UiMetrics.h`) for space. Window-level actions
  live in `MainWindow::MessageReceived` (`MSG_REC`, `MSG_METRONOME`). The
  keyboard-shortcuts help is a hard-coded `BAlert` string in `MSG_SHORTCUTS`
  (`src/ui/MainWindow.cpp:1516`) — the drift this item exists to end.
- **Meters.** `MeterView` (`src/ui/MeterView.{h,cpp}`) is the master meter in the
  transport bar: two horizontal bars fed from `Engine::PeakL/R()`
  (`src/engine/Engine.{h,cpp}`) by the ~60 Hz pulse in `MainWindow` (line ~1715,
  ~1787, ~1804). Per-track meters are drawn in `TimelineView::DrawHeader`
  (`src/ui/TimelineView.cpp:2089`), the mixer strips in
  `src/ui/MixerWindow.cpp:232`, the inspector's in `src/ui/InspectorView.cpp:313`
  — all through `DrawVUMeter` (`src/ui/Widgets.h:129`), which draws a *linear*
  bar with its own hard-coded gradient stops.
- **The one threshold set already exists**: `Theme.h` `kMeterWarn` /
  `kMeterClip` and `MeterColor()` (M1.2 fixed a real 1.0/0.7-vs-0.9/0.6
  disagreement). Do not add a second set; the kit-free maths takes the level as
  an argument and `Theme.h` stays the owner of the numbers.
- **RT rules** (`24-…-ENTRY.md` §7, `Engine.h:8`): the audio callback does
  arithmetic on preallocated memory, lock-free rings and atomics only — no
  allocation, locks, file I/O or logging. The master meter's readings are
  published the way the peaks already are (atomics written per block, read by
  the UI poll).
- `Loudness` (`src/dsp/Loudness.{h,cpp}`) is the worked example of kit-free
  meter maths with host tests: a measurement sink, preallocated in `Prepare`,
  RT-safe in `Process`, with `tests/loudness_tests.cpp` next to it.

## Goal

1. **One keyboard map.** A single kit-free table is the only place a binding
   exists. The arrange window's key handling resolves keys through it, and the
   shortcuts window is *generated* from it — so the help and the behaviour
   cannot drift. Home goes to the start, `R` records, `L` toggles loop, `C`
   toggles the click, Enter returns to zero, and everything the window already
   handled keeps working.
2. **Meters that say something.** The master meter becomes a dB-scaled meter
   with peak hold, a clip indicator that a click resets, an RMS line, and a
   correlation strip; every other meter (track headers, mixer strips, inspector)
   uses the same kit-free maths and the same one threshold set, so all of them
   agree. The kit-free maths is host-tested.

## Work items

1. **`src/model/KeyMap.h`** — kit-free, header-only (like `Grid.h`,
   `TempoMap.h`, `RecordPlan.h`), so the host tests and the UI read the same
   bytes:
   - `enum class KeyAction` — one id per thing the app does with a key:
     transport (play/stop, record, loop, click, return-to-zero), navigation
     (zoom in/out/fit, pan, page, marker prev/next), editing (delete,
     duplicate, clear selection, drop marker) and loop-between-markers.
   - `enum class KeyRoute { Dispatch, Menu }` — who runs it: our key handler,
     or a `BMenuItem` accelerator the window/menu dispatches (`None` action is
     only legal for a `Menu` row).
   - `struct KeyBinding { char key; char keyAlt; uint32 mods; KeyAction action;
     KeyRoute route; const char* keyName; const char* help; const char* group; }`
     and `kKeyBindings[]` as data. Key codes are the plain byte `KeyDown`
     delivers (BeOS codes for the special keys), with a comment; the UI adds a
     `static_assert` per special key so the table and the Interface Kit cannot
     disagree.
   - `const KeyBinding* FindKeyBinding(char key, uint32 mods)` — exact match on
     the modifier set (lock keys masked out), letters case-folded so `r`/`R` are
     one row and Shift is what makes `Shift-L` distinct from `L`.
   - `std::string BuildShortcutsText(const char* lineSep = "\n")` (or a
     `BuildShortcutsText(std::string&)` — keep it simple, STL only) — the help
     text generated from the same rows, grouped by `group`.
   - The table covers the plan's keys and everything `TimelineView::KeyDown`
     handles today, plus the menu accelerators as `Menu` rows (documented, not
     dispatched). The piano roll's editor-local keys (`1`–`7` tools, `q`,
     Cmd-A) stay in `PianoRoll::KeyDown`: they are view-local and the roll is a
     separate editor with its own toolbar — state that in the header comment.
2. **Behaviour reads the table** (`src/ui/TimelineView.cpp`): `KeyDown` looks
   the byte up in `kKeyBindings` and switches on `action`; no key literal is
   compared in the switch any more. New actions:
   - `ReturnToZero` (Home, Enter): playhead 0, scroll the view to the start,
     `kMsgSeek` — Home used to only scroll, which is the plan's complaint.
   - `RecordToggle`: post `kMsgTransportRecord` (new in `UiMetrics.h`);
     `MainWindow` runs the same body `MSG_REC` runs.
   - `LoopToggle` (L): flip `transport.loopEnabled`; turning it on with no valid
     range adopts the last ruler range, or the marker span when there is none
     (the existing `LoopBetweenMarkers`, now also on Shift-L so the feature is
     not lost to the new binding).
   - `ClickToggle` (C): post `kMsgClickToggle`; `MainWindow` runs the
     `MSG_METRONOME` body.
3. **The shortcuts window** (`src/ui/ShortcutsWindow.{h,cpp}`, new): a real
   `BWindow` (title "Keyboard Shortcuts") with a read-only `BTextView` named
   `"shortcuts"` filled from `BuildShortcutsText()`, sized to its content and
   clamped to the screen, themed through `Theme.h` tokens. Single instance, the
   way the mixer is (`kMsgShortcutsActivate`): `MSG_SHORTCUTS` shows and
   activates the open one instead of stacking another. The `BAlert` goes.
   `MainWindow` keeps the messenger.
4. **`src/dsp/MeterMath.h`** — kit-free, header-only:
   - the dB scale: `kMeterMinDb = -60`, `kMeterMaxDb = +6`,
     `LinToDb`, `DbToFrac`, `LinToFrac` (silence reads a finite floor);
   - `kMeterFullScale = 1.0f` — the definition of digital full scale, which
     `Theme.h`'s `kMeterClip` becomes (`= kMeterFullScale`), so the latch level
     and the meter's red zone are one number;
   - `PeakHold` — rise instantly, hold, then decay in dB/sec (deterministic in
     `nowUs`, host-testable);
   - `StereoMeter` — `Prepare(sampleRate, windowSeconds)` preallocates a
     fixed-size ring of per-block sums; `Process(stereo, frames)` (RT-safe
     after `Prepare`) accumulates L², R² and LR and publishes sliding-window
     RMS per channel and the correlation coefficient `Σlr / sqrt(Σl²·Σr²)`
     (0 with no signal, clamped to [−1, 1]);
   - `ClipLatch` — latch a peak at/over a given level until `Reset()`.
5. **Engine publishes the readings** (`src/engine/Engine.{h,cpp}`,
   `src/engine/Recorder.{h,cpp}`): one `StereoMeter` prepared in `Load` next to
   `fLoudness`, fed from the same post-master-gain block the peak loop already
   walks; `fRmsL`/`fRmsR`/`fCorr` atomics written per block, plus one clip
   flag word per meter owner (engine master, recorder input) that the RT side
   latches at full scale and the UI clears with `fetch_and` — a clip that
   happens between the UI's read and its clear is not lost. Accessors:
   `RmsL/RmsR/Correlation/ClipFlags/ClearClipFlags`. Still no allocation,
   locks or I/O in the callback.
6. **UI meters**:
   - `Widgets.h`: one `DrawMeterBar` (horizontal and vertical) — recessed well,
     fill mapped through the dB scale, coloured in three bands by `MeterColor`'s
     one threshold set (the gradient stops go), optional RMS line, optional
     peak-hold tick, optional clip lamp. Every meter calls it, so all of them
     read the same level the same way.
   - `MeterView` (master): dB-scaled L/R bars + RMS line + peak-hold tick +
     per-channel clip lamp + a correlation strip (bipolar, centre = 0, drawn
     inside the existing 124 px so the transport bar's layout is untouched).
     `MouseDown` anywhere on it resets the clip (and the view posts
     `kMsgMeterClipReset` so the engine's flag clears too). `MeterView::Readings()`
     and `ClipLatched()` are the test hooks. `MainWindow` feeds it
     `MeterReadings{peakL, peakR, rmsL, rmsR, corr, clipL, clipR}` at the three
     pulse sites (during a take the peaks come from the recorder, and RMS/corr
     are the engine's — say so in a comment).
   - `TimelineView` header meters, `MixerWindow` strips and `InspectorView` use
     the same helper (dB scale, one colour set); the mixer's fader-side scale
     becomes a real dB scale (−48…0) instead of the linear gain ticks it draws
     today.
7. **Tests**:
   - `tests/keymap_tests.cpp` (host): no duplicate (key, keyAlt, mods) row;
     every row has a non-empty key name, help and group; a `Dispatch` row has a
     real action and a `Menu` row has `None`; `FindKeyBinding` resolves the
     plan's five keys plus the existing ones and returns null for an unbound
     key; `BuildShortcutsText()` contains every row's key name and help line
     (so the help can never silently drop a binding).
   - `tests/meter_tests.cpp` (host): the dB mapping (0 dB → full-scale frac,
     −60 → 0, silence finite), peak-hold rise/hold/decay, the RMS window on a
     known sine (≈0.707·A) and on silence, correlation for identical (+1),
     inverted (−1), uncorrelated (≈0) and silent (0) input, the clip latch and
     its reset.
   - `tests/ui_functional_tests.cpp`: the real shortcuts window opened by
     `MSG_SHORTCUTS` with its text read back out of the real `BTextView`,
     single-instance; keys driven as `B_KEY_DOWN` messages (R/L/Home/Enter) with
     the model read back, including "a text field keeps its own keys"; a
     `B_MOUSE_DOWN` posted to the real `MeterView` clearing a latched clip after
     a full-scale take has set it; correlation/RMS read back from the view after
     playing an in-phase signal. `Shot()`s of the shortcuts window and of the
     running transport bar.
8. **Verification** (all in the record, with counts): host build exit 0 →
   `ctest` green → ASan/UBSan green → `haiku_syntax_check.sh` 0 FAIL (with
   `-DDAW_HAVE_LV2=1`; master's script does not define it, so the LV2 branches
   of `src/ui/` and the tests compile out — say in the record which way it was
   run) → VM `build` and `build-off` ctest → `ui_functional_tests` on the VM →
   the `DAW_UI_SHOTS` pass with every shot of the changed windows opened, at
   150% too (the transport bar and the new window are both geometry), in every
   theme mode this base has.

## Definition of done

- `MainWindow::MessageReceived`'s `MSG_SHORTCUTS` no longer holds a hand-written
  key list, and no key literal appears in `TimelineView::KeyDown`'s cases.
- Every new behaviour has a test that was mutation-checked (broken, watched
  fail, restored) and named in the commit body: the key table's duplicate rule,
  Home/Enter return-to-zero, R record, L loop, C click, the clip-latch reset on
  click, the dB mapping, the RMS window and the correlation value.
- No allocation, locks or I/O added to `Engine::FillBuffer` or the recorder's
  input hook; the added publishing is documented where it is written.
- Screenshots reviewed per `docs/UI_GUIDELINES.md`: the shortcuts window, the
  transport bar's meter (playing and clipped) and each meter site, in every
  mode this base has (it is pre-T1: one theme), with what was fixed named in
  the record.
