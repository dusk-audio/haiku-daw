# Record — M3.1 the transport bar (buttons, time display, load, status)

Branch `feature/m3-transport-mixer-jobs` (shared with 40 and 43), based on
`master` (`85b19b1`). Spec: `docs/agent-prompts/39-transport-bar.md`. Plan:
`docs/PLAN_1.0.md` §M3.1.

## Starting point — read this first

The worktree the orchestrator handed out was created at `785cd5a`, 364 commits
behind `master`, and did not contain a single file this brief names
(`src/ui/RenderJobs.cpp`, `ExportProgressWindow`, `KeyMap.h`, `PLAN_1.0.md`
itself). The branch was **fast-forwarded to `master` (`85b19b1`)** before any
work: `git merge --ff-only master`, a pure forward move of this branch only —
`master` was not touched, nothing was merged into it and no history was
rewritten. Every commit below sits on `master`'s content, and the brief's line
numbers (`MainWindow.cpp:2125` `LayoutTransportBar`, `:3613` `UpdateTimeReadout`)
all match.

## What landed

**The strip is two rows** (36 → 54 design px, `kDesignTransportH`). Nine
transport buttons — return-to-zero, rewind, fast-forward | Play, Stop, Record |
Loop, Click, Punch — then the zoom pair, the LCD well, and the existing
right-hand controls (tempo field, loudness, master volume, master meter).
Row B carries the status line. `TransportBar::ButtonRect` is the ONE geometry
function: the buttons are drawn from it, hit-tested with it, and the functional
test asks the bar for it (`MainWindow::TransportButtonRect`) instead of
computing an offset — the lesson `docs/HANDOFF.md` records about a synthetic
click landing on a control that had moved.

**Every button shows state that already existed**: `fPlaying`, `fRecMode`,
`transport.loopEnabled`, `fTransportCtl.fMetronome`, `transport.punchEnabled`.
Loop runs `TimelineView::ToggleLoop()`, return-to-zero runs
`TimelineView::ReturnToZero()` (both made public for exactly this — one
implementation, two ways in), and Click posts the same `kMsgClickToggle` the C
key does. Rewind/fast-forward move the playhead one bar through the tempo map's
own `BarBeat` + the new `FrameAtBarBeat`.

**The time display** cycles BBT → min:sec → samples → timecode on a click and
takes a typed position on a double-click. The four formats and their inverse are
kit-free and host-tested (`src/model/TimeDisplay.h`, `tests/time_display_tests.cpp`):
BBT is `bar.beat.tick` over the meter map (the inverse of `TempoMap::BarBeat`,
checked across a meter change), timecode is 30 fps non-drop. The window owns the
mode; the view only shows the string it is handed and the typed text it collects.

**CPU/DSP and xruns**: `Engine::PlayTrampoline` times the callback *around*
`FillBuffer` (the callback body itself stays clock-free — the RT rule) and
publishes an atomic plus a peak hold the pulse takes and clears; the readout is
the *peak* block's share of the buffer duration. `TrackStream::Mix` counts one
xrun per block whose ring was starved (the `fSkipDebt` path), the stream holding
a pointer to the engine's counter; the readout adds `Recorder::Xrun()`.

**Status line**: `TransportBar::SetStatus`/`SetInfo`, both strings owned by the
window (it is the only place that knows the tempo map, the engine and the jobs).
The status is the transport's *state* ("Stopped", "Playing", "Recording
take-3.wav", "Monitoring input"), so it cannot go stale; a job or a bad typed
entry replaces it until the state changes again.

## Verification (this host)

| Command | Result |
|---|---|
| `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` | exit **0** |
| `cmake --build build-host -j2` | exit **0** |
| `ctest --test-dir build-host` | **69/69 passed**, exit 0 |
| `sh scripts/haiku_syntax_check.sh` | **75 OK, 4 FAIL** — see below |

The 4 FAILs are `src/engine/FlacSink.cpp`, `FlacSource.cpp`, `VorbisSink.cpp`,
`VorbisSource.cpp`: `fatal error: FLAC/stream_encoder.h: No such file or
directory`. Those files and their headers are byte-identical to `master`'s
(`git diff --name-only master...HEAD` lists none of them); the cross toolchain
under `~/haiku-cross/` carries no FLAC/vorbis headers, so the check cannot pass
them on this host. **All 24 files this branch touches pass** (0 FAIL among
them).

`tests/ui_functional_tests.cpp` gained `TestTransportBar`: every button clicked
at its own reported rect, each effect asserted (play/stop, loop, metronome,
punch, RTZ, rewind/forward by bar, zoom), the mode cycle, a typed `1:30` seek, a
bogus entry that must change nothing, and the readout line's "BPM/DSP/XR".

## Mutation checks (each one watched fail, then restored)

| # | Mutation | Caught by |
|---|---|---|
| M1 | `NextTimeMode` returns the same mode | `time_display_tests` (cycle) |
| M2 | `BarBeatToBeat` ignores the meter map | `time_display_tests` (meter-change round trip) |
| M3 | min:sec formatter doubles the seconds | `time_display_tests` |
| M4 | timecode parses frames at 25 fps | `time_display_tests` |

The UI half of this item (the buttons' geometry, the lamps, the layout) runs
only on the VM: it is covered by `TestTransportBar`, which the orchestrator
runs, and no mutation of that half can be executed here. Said plainly rather
than papered over.

## Not verified here

- **The screen.** `docs/UI_GUIDELINES.md` §1 is unmet by this package: the
  screenshot pass (`DAW_UI_SHOTS`, both themes), the click tests and the
  audible checks are the orchestrator's. The layout is written to survive it
  (no overlap at any width, every string truncated to its rect, `Themed()`
  metrics throughout) and the hide order is documented in
  `LayoutTransportBar`, but "looked at" is a claim only the person with the VM
  can make.
- **The DSP/xrun figures under real load** (the VM's 2 vCPU): the numbers are
  read from engine atomics and the readout is asserted to exist, not to hold a
  particular value.

## Deviations from the spec

- The spec's hide order ("loudness → master slider → tempo field") is
  implemented as a right-to-left placement: the tempo field and the loudness
  readout share row A, the master volume sits in row B, each placed only when
  what is left of it still clears x 602, and the LCD is hidden when the
  master meter would cover it. At the app's own 1000 px default everything
  shows.
- The typed entry commits on Enter and is abandoned by a click elsewhere;
  Escape was dropped (a `BTextControl`'s inner text view consumes it), and the
  spec's line about Escape is corrected there.
