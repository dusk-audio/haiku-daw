# Task: M3.1 — the transport bar (buttons, time display, CPU/xrun, status)

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/m3-transport-mixer-jobs` off `master`. This is item M3.1 of
`docs/PLAN_1.0.md` (M3.1, "Transport bar"); M3.2 (the keyboard map) is already
in `master`, and these buttons bind to the SAME state the keys already drive.

## Codebase orientation (read before coding)

- `src/ui/TransportBar.{h,cpp}` — the strip below the menu bar: a custom-drawn
  `BView` that today draws Play / Stop / Rec (each lit from a bool) and a zoom
  `-`/`+` pair, and posts one message per button to `MainWindow`. Its children
  (the time readout, the loudness readout, the master slider, the tempo field,
  the master meter) are placed by `MainWindow::LayoutTransportBar`
  (`MainWindow.cpp:2125`), a design-offset layout with a documented
  hide-order for narrow windows (loudness, then tempo field, then slider).
  The bar's own height is `kDesignTransportH` (36 design px), pinned by
  `MinSize()`/`MaxSize()` — a plain `BView` in a layout without a maximum
  soaks up spare height and grows a dead band (the M1.3/M1.4 defect).
- **The state already exists; do not invent any.**
  - playing / recording: `TransportController::fPlaying`, `RecordController::fRecMode`;
  - loop: `project.transport.loopEnabled` (+ `loopStart/loopEnd`), toggled by
    `TimelineView::ToggleLoop()` (key L) and the ruler drag;
  - metronome: `TransportController::fMetronome`, toggled by the View menu item
    and `kMsgClickToggle` (key C);
  - punch: `project.transport.punchEnabled/punchIn/punchOut` (serialized as the
    `punch` line, honored by `RecordController`), toggled by the ruler's
    punch drag;
  - return to zero: `TimelineView::ReturnToZero()` (Home / Enter) — make it
    public rather than re-implementing it.
  The keyboard map that drives them is the kit-free table `src/model/KeyMap.h`,
  and `tests/keymap_tests.cpp` holds its rules.
- `MainWindow::UpdateTimeReadout(Frame)` (`MainWindow.cpp:3613`) formats the
  bar's readout today: `tempoMap.BarBeat()` + `mm:ss.mmm` into one string.
- The engine measures the callback it runs: `Engine::FillBuffer`
  (`Engine.cpp:1181`, entered from the `BSoundPlayer` callback at `:1170`) and
  the graph build time in `Engine::BuilderLoop` (`:1085`, `LastBuildUs`).
  **`FillBuffer` and everything it calls may not allocate, lock, log or do file
  I/O** (`docs/PLAN_1.0.md`, hard rules) — the RT thread only writes atomics
  that the window's 60 Hz pulse reads.
- Tests: `tests/ui_functional_tests.cpp` drives the real window by posted
  messages on the VM (kit-free code is host-tested with `ctest --test-dir
  build-host`). New kit-free math needs a host test; the VM run is the
  orchestrator's.
- **UI work is done only when its screenshots have been looked at**
  (`docs/UI_GUIDELINES.md` §1). The implementer of this package cannot run the
  VM; the orchestrator runs the `DAW_UI_SHOTS` pass. Everything below is written
  to be looked at: no overlap, no dead band, text truncated to its rect
  (`TruncateString`), every new control named on screen, `Themed()` metrics.

## Goal

The transport strip is a commercial DAW's transport: the full button row (each
showing its state), a large time display that cycles BBT → min:sec → samples →
timecode and takes a typed position, a tempo/meter display, a DSP-load readout
with an xrun counter, and a status line.

## Design

### 1. The strip becomes two rows (36 → 54 design px)

One row cannot hold nine buttons, a large display, the master fader and the
meter at the app's own 1000 px default window (`src/main.cpp:140`). The strip
therefore gets a buttons row and a status row; the master meter spans both.

Row A (y 5..31): RTZ, Rewind, Fast-forward, Play, Stop, Record, Loop, Click,
Punch (28 px wide, 22 tall, in three groups: locate / transport / modes), then
the zoom pair, then the LCD well (the big time display), then the existing
right-hand controls (Vol + master slider, BPM + tempo field) and the master
meter pinned to the right edge.

Row B (y 33..51): the status line — left, the last thing that happened ("Playing",
"Recording take-1.wav", "Exported mix.wav", an error); right, the readouts
"120.0 BPM  4/4   DSP 12.0%  XR 0" and the loudness readout.

`LayoutTransportBar` keeps its shape (design offsets, clamped to the window
width, a documented hide order for narrow windows) and gains the LCD's own
rule: the time display needs at least a legible width, so the hide order
becomes loudness → master slider → tempo field. Nothing overlaps at any width:
each item is placed only when its rect fits left of the next one.

### 2. Buttons

Each button draws its own state (`SetPlaying`, `SetRecording`, plus
`SetLoop`, `SetClick`, `SetPunch`), posts one message, and is hit-tested against
the same rect it is drawn with (one function, so drawing and clicking cannot
disagree). The messages the window already handles are reused
(`kMsgTransportRecord`, `kMsgClickToggle`); new ones are added for the actions
that had no message (RTZ, rewind, fast-forward, loop, punch) in the same
`UiMetrics.h` block the keyboard map's messages live in.

- Rewind / fast-forward: the playhead to the previous / next bar, through the
  tempo map's own beat math (`TempoMap::BarBeat` + `FrameAtBarBeat`, see below),
  applied with `TimelineView::SeekTo`.

### 3. The time display (kit-free core + a custom view)

- `src/model/TimeDisplay.h` (new, kit-free, header-only, host-tested): the four
  formats and their inverse —
  `FormatTimePosition(mode, frame, sampleRate, tempoMap)`,
  `ParseTimePosition(mode, text, sampleRate, tempoMap, &out)`,
  `NextTimeMode(mode)`, `FrameAtBarBeat(tempoMap, bar, beat, tick)`.
  BBT reads `bar.beat.tick` (480 ticks per beat), min:sec reads `m:ss.mmm`,
  samples reads the frame count, timecode reads `HH:MM:SS:FF` at 30 fps
  (non-drop). The parse accepts the loose forms a user types ("3.2", "1:23",
  "1:23.5", "12345", "00:01:23:15") and rejects anything else — the caller
  shows the failure in the status line rather than seeking to a wrong place.
- `TransportTimeView` (in `src/ui/TransportBar.{h,cpp}`, the bar's file): draws
  the big position plus a small mode tag, `B_NAVIGABLE` so it can take focus.
  A single click posts `kMsgTimeCycle` (the WINDOW advances the mode and pushes
  the formatted text back, so the mode is one piece of state, not two); a
  double click starts typed entry — the view takes focus, shows what is being
  typed with a caret, Enter posts `kMsgTimeEntry` with the string and the
  mode, and a click anywhere else abandons it. (Escape is not handled: a
  `BTextControl`'s inner text view takes it before the view sees it. The typed
  entry is a `DawTextField` in the mixer's case and this view's own buffer
  here, and neither route gets the Escape.) The window parses, seeks
  (`TimelineView::SeekTo`) and reports a bad entry in the status line.

### 4. CPU/DSP load and the xrun counter

- `Engine::FillBuffer` is not allowed a clock. The measurement brackets the call
  in the `BSoundPlayer` callback body (`Engine::RenderCallback`, `Engine.cpp:1170`)
  — `system_time()` before and after, stored in `std::atomic<bigtime_t>
  fLastCallbackUs`, with `Engine::LastCallbackUs()` for readers. `FillBuffer`'s
  own body gains no syscall; the rule stays intact.
- The DSP figure the window shows is `fLastCallbackUs / (bufferFrames / rate)`
  as a percentage, clamped to 0..999, and it is a **peak hold over the last
  second** of callbacks (an average hides the spikes that cause an xrun):
  the engine keeps a second atomic, `fPeakCallbackUs`, that the RT thread
  updates with a max and the window's pulse resets after it has read it —
  both plain atomics, no locks.
- Xruns: `Engine` gains `std::atomic<uint64_t> fXruns`, incremented ONCE per
  `TrackStream::Mix` call that found the ring starved (`Engine.cpp:322` — the
  `fSkipDebt` path; the stream carries a pointer to the engine's counter, set at
  build, so `Mix` needs no engine reference). A stream that underran is the
  audible failure the counter is for. The readout adds the recorder's own
  `Recorder::fXrun` (latched, one per take) so a take that dropped samples is
  visible too.

### 5. Status line

`TransportBar::SetStatus(const char*)` and `SetInfo(const char*)`: the window
owns both strings (it knows the tempo, the meter, the load and what just
happened) and the bar draws them, truncated to their rects. `MainWindow` sets
the status on the paths a user would want confirmed — play, stop, record
start/stop, an export or region job finishing, a save — and on a failed time
entry.

## Work items

1. `src/model/TimeDisplay.h` (new) — the four formats, the parse and
   `FrameAtBarBeat`; header-only like `KeyMap.h` so the UI and the tests read
   the same code.
2. `src/engine/Engine.{h,cpp}` — `LastCallbackUs()`, `PeakCallbackUs()` +
   `ResetPeakCallbackUs()`, `Xruns()`, the RT-side updates described above, and
   the `TrackStream` counter pointer. No allocation, lock or clock inside
   `FillBuffer`.
3. `src/ui/TransportBar.{h,cpp}` — the new buttons with their state setters and
   hit tests, the two-row draw, `TransportTimeView`, and the status/info
   setters. All geometry from `Themed()` design constants.
4. `src/ui/MainWindow.{h,cpp}` — the bar's construction with the new message
   ids, `LayoutTransportBar` for the two rows, the new message handlers
   (time cycle / time entry / RTZ / rewind / fast-forward / loop / punch),
   the per-pulse `SetStatus`/`SetInfo`/state push, and a `TransportStates()`
   test hook. New constants in a clearly separated block at the end of the MSG
   enum; new members grouped at the end of the class.
5. `src/ui/TimelineView.h` — `ToggleLoop()` and `ReturnToZero()` become public
   (the bar's buttons run exactly what the keys run).
6. Tests: `tests/time_display_tests.cpp` (new, host: round-trips of all four
   formats, the parse's accept/reject set, BBT across a meter change) and VM
   checks in `tests/ui_functional_tests.cpp` (the buttons post what they should,
   the displayed mode cycles, a typed position seeks, a bad entry changes
   nothing, the xrun counter moves when a stream underruns).

## Out of scope (say so in the record)

- The Preferences window (M3.5) keeps the buffer-size/count-in radios; the
  Audio menu is unchanged.
- No new meters (M3.3 is merged); the loudness readout just moves to the status
  row.
- The typed time entry does not accept a timecode frame rate other than 30 fps.

## Definition of done

- `cmake --build build-host` exits 0, `ctest --test-dir build-host` green (count
  in the record), `sh scripts/haiku_syntax_check.sh` 0 FAIL.
- Every new behaviour has a test, mutation-checked (mutations named in the
  record).
- The record names what is NOT verified: the screenshots and the VM run belong
  to the orchestrator (this package cannot reach the VM).
