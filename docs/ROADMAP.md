# Haiku DAW — Roadmap

Living plan: what's built, what's left, in staged order. Update as phases land.
See `HANDOFF.md` for current state detail and `ARCHITECTURE.md` for the design.

## Done (foundation is solid)

- **Engine** — `BSoundPlayer` output, per-track bus mixing, per-stream resampling
  to the output rate, per-track effect chain, master gain, metronome,
  user-adjustable output buffer. RT-safe callback; disk threads do I/O.
- **Model** — `Project`/`Track`/`Clip`/`MidiNote`, command stack (undo/redo),
  `EffectDesc` (dynamic param vector), `Grid` (bars/beats + snap), `ProjectIO`
  text save/load with project-relative media paths, `AutomationLane` (built +
  tested, **not yet wired**).
- **Audio** — clip playback with fades / pan / gain / mute / solo, loop, seek,
  recording (BMediaRecorder → WAV take), offline WAV export (bounce).
- **MIDI** — internal sine `Synth`, piano-roll notes (add/move/resize/velocity).
- **Effects** — parametric 5-band EQ, Delay, Reverb, Compressor, Saturator,
  Gate, Widener (+ legacy Biquad); effects editor (add/remove/reorder/params),
  ported from the user's Multi-Q / tape-echo / multi-comp DSP.
- **UI** — timeline (bar/beat ruler, lanes, clips + waveforms, piano roll),
  transport, menu bar, mixer window, track add/delete/rename, clip drag
  (horizontal + cross-track ghost), copy/paste, import audio, zoom/pan, snap,
  tempo field, master volume, buffer-size menu.

## Phase A — Mixer routing  ← recommended next

The biggest architectural gap: routing is flat (every track → master sum; the
"master" is just a gain + meter).

- **A1. Master-bus FX** — give the master its own `EffectDesc` chain between the
  sum and output, plus its meter/gain. Reuses the effects editor. *Small.*
- **A2. Group / bus tracks** — a bus track type + per-track `output` routing
  (master or a bus id); engine sums in dependency order (tracks → bus →
  master). Model + engine + mixer UI. *Large.*
- **A3. Aux sends** — per-track send levels to aux buses (pre/post-fader); an
  aux bus carries shared FX (e.g. one reverb) → master. *Medium.*

## Phase B — Automation (model already built + tested)

- **B1.** Wire `AutomationLane` into `Track`: per-track gain/pan lanes and
  per-effect-param lanes; serialize in `ProjectIO`.
- **B2.** Engine reads the lane value at the playhead each block and applies it
  (RT-safe; the model owns the breakpoints, the engine reads a snapshot).
- **B3.** UI automation lanes under a track: draw + add/move/delete breakpoints.

## Phase C — Editing depth

- Clip split, crossfade between overlapping clips, per-clip gain.
- Multi-select + range copy/paste/duplicate/delete.
- Track reorder, per-track height + color.

## Phase D — Recording depth

- Input monitoring toggle, count-in, punch-in/out, loop-record + take comping.
- MIDI recording (needs external MIDI input or step entry).

## Phase E — MIDI depth

- Richer instrument: multi-waveform + ADSR UI, then a sample/wavetable synth.
- **External Midi Kit 2 I/O** (keyboards, hardware synths) — *deferred until the
  VM can pass through MIDI; `scripts/midi_probe.sh` is ready to characterize it.*

## Phase F — Sample browser

- BFS attribute tagging (BPM / Key / Duration) + a live `BQuery` browser — the
  native-Haiku superpower (ARCHITECTURE §5.2).

## Phase G — UI / UX polish pass

Dedicated pass once features settle: consistent spacing/theming, styled widgets,
resizable/scrollable panels (vertical track scroll), zoom-to-fit, a keyboard-
shortcut map, better meters, tidy dialogs.

## Phase H — Persistence & robustness

- Save app settings (buffer size, last dir) and window layout.
- Project bundle: keep recorded takes next to the `.dawproj`.
- Autosave / crash recovery.

## Phase I — Plugins (long-term)

Internal effect add-on ABI, then LV2 hosting with native generic GUIs
(ARCHITECTURE §8 stages 2–3). VST is a maybe.

---

### Suggested order

A (routing) → B (automation) → C (editing) → D (recording) → G (a polish pass) →
F (browser) → E2/D2 (external + MIDI recording, once MIDI is testable) → I
(plugins). E1 (better synth) and H (persistence) can slot in opportunistically.
