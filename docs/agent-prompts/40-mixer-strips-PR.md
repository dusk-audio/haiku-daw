# Record — M3.4 the mixer strips (trim, phase, sends, solo groups, dB entry)

Branch `feature/m3-transport-mixer-jobs` (shared with 39 and 43), based on
`master` (`85b19b1`). Spec: `docs/agent-prompts/40-mixer-strips.md`. Plan:
`docs/PLAN_1.0.md` §M3.4.

## What landed

**Model** — `Track` gains `trim` (linear, default 1.0), `phaseInvert` and
`soloGroup`, beside the fader/mute/solo fields they sit next to on a console.
Every edit goes through the `CommandStack`: `SetTrackTrimCommand` (clamped to
`kTrackTrimMax` = +12 dB, coalescing so a knob drag is ONE undo step),
`SetTrackPhaseCommand`, `SetTrackSoloGroupCommand`. `SetTrackSoloCommand` gained
the solo-group cascade — the counterpart of the mute-group cascade it has had
since the strip's first version — with every affected member's prior flag stored
for Undo. With `soloGroup == 0` (every existing project) the behaviour is
exactly what it was.

**Persistence** — three append-only optional lines (`tracktrim`,
`trackphase`, `tracksologroup`), written only when the field is not its default,
addressed by track id and applied after the whole file is read (the `trackfold`
pattern). A project that never touches them writes the legacy bytes; an older
file loads with the defaults; a hand-edited out-of-range trim is clamped rather
than failing the load.

**Engine and exporter** — `TrackStream` gets `fTrim`/`fPhase` atoms and folds
them into the same per-block multiply its fader uses; the `Graph::Bus` gains the
same pair for MIDI and bus nodes, applied where that node's own material is
scaled. Keeping them *separate from* the fader atoms is the point: gain/pan
automation overwrites those every block and would otherwise drop a trim. The
exporter multiplies the same two factors into its per-track fader, in both the
constant and the automated branch, so a bounce lands at the level the transport
plays.

**Sends** — one row per aux send (up to two, "+N" beyond that opens the sends
editor): the destination's short name, a level knob, the level as text. The
knob posts `kMsgMixSend`, which builds a `SetSendsCommand` — the sends editor's
own path, not a second one — and, like the sends editor's edits since A3, it
takes effect on the next Play (the engine's send tables are built with the
graph; rebuilding per knob move would rebuild a 320-clip project dozens of times
per gesture).

**Strip UI** — an input-trim knob with its value in dB and a phase `Ø` button in
one row above the pan knob; the `G` box beside Solo cycles the solo group; and
double-clicking the fader or the value readout opens an inline dB field over the
strip's value row (Enter applies through the same `kMsgApplyMix` macro; a click
elsewhere abandons it; a value that cannot be read changes nothing).

The trim knob's mapping (unity at 12 o'clock, the ceiling at the top of the
sweep, silence at the bottom) and the dB entry's parse are kit-free in
`src/dsp/MeterMath.h`, so the knob's arithmetic is host-tested rather than only
exercised by a mouse.

## Verification (this host)

| Command | Result |
|---|---|
| `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` | exit **0** |
| `cmake --build build-host -j2` | exit **0** |
| `ctest --test-dir build-host` | **69/69 passed**, exit 0 |
| `sh scripts/haiku_syntax_check.sh` | **75 OK, 4 FAIL** (the 4 pre-existing FLAC/vorbis files, untouched here — see `39-transport-bar-PR.md`) |

New checks: `model_tests` +24 (trim clamp/coalesce, polarity, the solo cascade
and its undo, the group-cleared path), `projectio_tests` +29 (round trip, no
legacy noise, patched old file, clamping), `exporter_tests` +7 (a half trim
halves the bounce, a polarity flip inverts it, a 0.5 send lands halfway),
`meter_tests` +27 (the dB parse's accept/refuse set, `GainFromDb`, the trim
knob's mapping and its round trip), and `TestMixerStripExtras` in
`tests/ui_functional_tests.cpp` (every intent through its own message, the
solo-group cascade end to end, the strip snapshot after a refresh).

## Mutation checks (each one watched fail, then restored)

| # | Mutation | Caught by |
|---|---|---|
| M5 | `SetTrackTrimCommand` drops the ceiling | `model_tests` (`trim == kTrackTrimMax`) |
| M6 | `SetTrackSoloCommand` ignores the solo group | `model_tests` (the cascade) |
| M7 | the loader ignores `trackphase` | `projectio_tests` (round trip) |
| M8 | the writer never emits `tracktrim` | `projectio_tests` (round trip) |
| M9 | the exporter drops trim/polarity | `exporter_tests` (half-trim bounce) |
| M10 | `ParseDecibels("-inf")` returns 0 dB | `meter_tests` |
| M11 | `GainFromDb` loses its silence floor | `meter_tests` |
| M12 | `TrimToKnob` no longer centres unity | `meter_tests` |

The strip's drawing and mouse gestures (the knob drags, the `Ø`/`G` boxes, the
inline field) run only on the VM; the functional test drives the same messages
those gestures post, and no mutation of the drawing half can be executed here.

## Not verified here

- **The screen.** Screenshots (`DAW_UI_SHOTS`, both themes, 100% and 150%) are
  the orchestrator's — including whether the strip's new rows read well at the
  mixer's minimum height. Two sends + the trim row push the fader down; the
  block is reserved per rack (like the insert rows) so the pan knobs and faders
  stay on one line, but only the eye can say it looks right.
- The audible difference of a trim/phase change *while playing* on the VM: the
  engine path is the same one the exporter's test proves on the host, and
  `UpdateMix` (the 60 Hz pulse) is what carries it live.

## Deviations from the spec

- The inline dB field commits on Enter and is abandoned by a click elsewhere;
  Escape is not handled (the field's inner `BTextView` takes it first). The
  spec's line is corrected there.
- Send levels take effect on the next Play, matching the sends editor's own
  behaviour, rather than forcing an engine rebuild per knob move. Put plainly:
  a send-level change is not audible until the transport is restarted.
