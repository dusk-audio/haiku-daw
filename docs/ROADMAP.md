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

## Phase A — Mixer routing  ✅ DONE

Routing is now a real graph (tracks → buses → master) resolved by topological
sort, with sends, a monitor section, and BS.1770 metering.

- **A1. Master-bus FX** ✅ — master `EffectDesc` chain between the sum and
  output, edited via the effects editor (master sentinel target).
- **A2. Group / bus tracks** ✅ — `TrackType::Bus` + per-track `output`; engine
  and offline Exporter both process nodes in dependency order (RoutingGraph
  topo). New Bus menu item + header routing popup.
- **A3. Aux sends** ✅ — per-track `sends[]` (dest bus, level, pre/post-fader);
  `ResolveOrderWithEdges` adds send edges to the topo. Exporter honors pre/post
  exactly; the RT engine does post-fader (live). SendsWindow editor + "Snd"
  header box. Host-tested.
- **A4. Monitor section** ✅ — master Dim (~-20 dB) + Mono, RT-safe atomics
  applied after metering (meters show the true mix). View menu toggles.
- **A5. Master metering** ✅ — BS.1770-4 loudness meter on the mix: momentary /
  short-term LUFS + true-peak dBTP, live readout in the transport bar. Integrated
  disabled on the audio thread (RT-safe); available offline.

## Phase T — Tempo & meter map  ✅ DONE (step changes)

- ✅ Kit-free `TempoMap` (T1): frame-anchored tempo + meter changes, exact
  frame<->beat integral, meter-aware BBT. Project field + IO. Host-tested.
- ✅ Grid/ruler/snap (T2): beat lines + snap walk the map (variable spacing).
- ✅ Metronome (T3): clicks follow tempo changes, accent follows meter.
- ✅ UI (T4): right-click the ruler to add/remove tempo (preset BPM) + meter
  (preset signature) changes; amber/blue markers on the ruler.
- *Deferred:* tempo ramps (accel/rit); BBT transport readout.

## Phase B — Automation  ✅ DONE (gain + pan)

- **B1.** ✅ `gainAuto`/`panAuto` `AutomationLane` on `Track` (absolute
  envelopes; empty = static fader). `SetAutoLaneCommand` (snapshot gesture,
  undoable). `ProjectIO` serializes `auto gain|pan` lines. Host-tested.
- **B2.** ✅ Node-level fader: the offline Exporter applies a per-sample
  gain*pan envelope (host-tested ramp); the RT engine drives the gain path per
  block from an RT-owned lane snapshot (`ValueAt(blockStart)`), UpdateMix
  refreshes only audibility for automated tracks.
- **B3.** ✅ Header "Auto" box (Off/Gain/Pan/**fx-param lanes**); the lane
  content area draws the curve and edits breakpoints (click add / drag move /
  right-click delete), one undoable gesture per drag.
- **B4.** ✅ Effect-parameter automation: `IEffect::SetParam` (RT-safe) across all
  effects; `Track.fxAuto` lanes (fxIndex+slot); engine + Exporter drive params
  per block; right-click a knob in the effects editor to create a lane, edit it
  via the timeline Auto box. Host + VM-verified.
- ✅ Undo unified: fx / sends / instrument / notes / color / height edits are now
  undoable (command coalescing folds a drag into one step).
- *Deferred:* send-level automation lanes.

## Phase C — Editing depth  ✅ DONE

- ✅ Per-clip gain (C1): `Clip.gain`, engine+Exporter apply, IO, Ctrl-drag +
  dB label.
- ✅ Clip split (C2): `SplitClipCommand`, right-click "Split here".
- ✅ Track reorder (C3): `MoveTrackCommand`, name menu Move Up/Down.
- ✅ Per-track color + height (C4): 6-color palette + variable lane height
  (LaneRect sums heights); name menu Next Color / Taller / Shorter.
- ✅ Crossfade (C5): kit-free `ComputeCrossfades` — overlapping clips auto
  fade-out/in; engine + Exporter derive effective fades. Host-tested. The
  timeline now *draws* it too (it was audible but invisible): clips render their
  effective fades, and the overlap gets a tinted band + crossing ramps. Both the
  renderers and the UI derive the span from one shared `CrossfadeOverlap`, so
  what you see can't drift from what you hear.
- ✅ Multi-select + range ops (C6): clip selection (click / Shift-click /
  rubber-band / Esc), group move, Delete, Ctrl-D duplicate — each one undo step
  via `MacroCommand`. (MIDI-note multi-select deferred.) MIDI regions also
  group-move as a unit.
- ✅ Region ops (C7): audio-clip right-click Normalize (peak → clip gain),
  Reverse (render a mirrored WAV), Strip Silence (split at ≥250 ms gaps),
  Clear Fades. Kit-free DSP in `model/RegionOps` (host-tested).
- ✅ Freeze / bounce a track (C8): render a track through its fader + FX to a
  stereo WAV (isolated routing) and swap in one clip with a flat fader;
  Unfreeze restores the stashed pre-freeze content. `FreezeTrackCommand`
  (host-tested), reuses the offline `Exporter`.

## Phase D — Recording depth  ✅ DONE (audio; runtime-verify on VM)

Recording now runs the playback engine simultaneously (overdub). Kit-free timing
math in `RecordPlan.h` (host-tested); Media-Kit orchestration compile-checked
with the local cross-compiler, runtime pending on the VM.
- ✅ Count-in (D1): Audio > Count-in (0/1/2 bars), tempo-map aware; engine plays
  N bars of click into the record point, capture begins there.
- ✅ Punch-in/out (D2): Ctrl-drag the ruler sets a punch range; the take is
  trimmed to it (`PunchedTake`, non-destructive).
- ✅ Input monitoring (D3): `IMonitorSource` — Recorder pushes a lock-free
  monitor ring the engine mixes in. Rate-matched only (no RT resample yet).
- ✅ Loop-record + take comping (D4): loop + Rec stacks per-pass takes
  (`LoopTakes`) as a take group; only the active take sounds; right-click ->
  Next Take. `takeGroup`/`takeActive` on Clip.
- *Deferred:* MIDI recording (no VM input path); region-level comping;
  resampled input monitor.

## Phase E — MIDI depth  ✅ DONE (instrument; MIDI-in deferred)

- ✅ Richer instrument: per-MIDI-track `Instrument` — waveform (sine/saw/square/
  triangle) + ADSR with a release tail past note-off. Stateless Synth; engine +
  Exporter + IO wired; `InstrumentWindow` editor from the "Inst" header box.
  Host-tested + VM-verified.
- ✅ Band-limited oscillators: saw/square use PolyBLEP (no aliasing on high
  notes); triangle/sine clean. *Deferred:* sample/wavetable synth.
- ✅ SMF (`.mid`) import/export: kit-free `model/SmfIO` (format 0/1, varlen,
  running status, tempo + name meta; host round-trip tested). File menu Import
  MIDI / Export MIDI; ticks<->frames at the project tempo.
- **External Midi Kit 2 I/O** (keyboards, hardware synths) — *deferred until the
  VM can pass through MIDI; `scripts/midi_probe.sh` is ready to characterize it.*

## Phase F — Sample browser  ✅ DONE

- ✅ BFS attribute tagging: `src/storage/BfsAttr` reads/writes DAW:bpm / DAW:key
  / DAW:duration + creates their fs indexes; import auto-tags duration.
- ✅ Live `BQuery` browser (`SampleBrowser`, View > Sample Browser): name filter
  over the boot volume, lists audio files + their duration/BPM, double-click
  imports, BPM field tags the selection. Compile-checked + VM-built.
- ✅ Drag-and-drop into the timeline: the arrangement accepts `B_SIMPLE_DATA`
  drops from Tracker/the desktop — `.wav` imports onto the track under the
  cursor at the snapped drop frame (multi-file drops walk down consecutive
  lanes), `.mid` goes through `ImportMidi` at the drop position. Other
  extensions are logged and skipped.
- *Later:* BPM/key range predicates; Tracker MIME attr registration so columns
  show there too; `RefsReceived` on the app so icon/Deskbar drops and "Open
  With" work too (today only drops onto the open window do).

## Phase G — UI / UX polish pass  ✅ DONE

- ✅ Vertical track scroll (mouse wheel + PageUp/Down), zoom-to-fit (F), BBT
  transport readout, keyboard-shortcuts help.
- ✅ Pro-DAW visual pass: cohesive dark palette; track headers gain a color
  stripe + right-edge stereo meter; **per-track metering** (engine exposes
  per-node peaks, RT-safe).
- ✅ Mixer rewritten fully custom-drawn (no light OS widgets): faders w/ unity
  tick + dB, stereo meters, pan, M/S, and a master strip; live peaks pushed.
- ✅ **Piano-roll MIDI editor** (`PianoRoll`): keyboard column + pitch×time grid,
  tempo-map grid/snap, add/move/resize/delete/velocity; opens on MIDI
  track-name double-click. Opens focused on the tapehead when the playhead is
  inside the region (else at the region head). Note creation belongs to the
  Pencil/Brush tools only — the Pointer selects and marquees.
- ✅ Code-review pass over the whole codebase: fixed EQ shelf NaN, delay
  runaway feedback, self-send routing collapse, MacroCommand failed-subcommand
  undo (data loss), AppSettings zero-clobber, WavSource OOB, WavWriter race,
  monitor UAF — all with regression tests.
- ✅ Effects editor rewritten custom-drawn: dark rotary knobs (no OS sliders),
  **EQ frequency-response graph** with draggable per-band handles
  (`Eq::MagnitudeResponseDb`), **compressor transfer curve**, per-effect
  reorder/remove/add, vertical scroll.
- ✅ Wheel-adjust in the effects editor: the wheel over a knob's dial nudges it
  (Shift = fine), and over an EQ band handle nudges that band's **Q** — the one
  band parameter the freq×gain graph drag can't reach. Hit-tested against the
  drawn dial, not the (much larger) click-drag cell, so scrolling the panel list
  doesn't nudge parameters in passing. `SetFxCommand` takes one command per
  gesture, so notches preview live and a debounce timer folds them into a single
  undo step (flushed on window close so the model can't lag the engine).
- ✅ Transport: custom dark Play/Stop/Rec buttons (lit by state) + themed Vol
  fader.
- *Later:* live compressor GR meter (needs per-effect engine telemetry), tidy
  dialogs, drag-resize panels.

## Phase H — Persistence & robustness  ✅ DONE

- ✅ App settings + window layout: kit-free `AppSettings` (buffer, count-in,
  metronome, monitor-input, last dir, window frame); loaded on start, saved on
  quit to `~/config/settings/HaikuDAW/settings`. Host-tested.
- ✅ Take bundle: recorded takes written into the project's directory (portable
  bundle) when saved, else CWD.
- ✅ Autosave / crash recovery: a recovery `.dawproj` every 30 s; clean quit
  deletes it, so a leftover triggers a Recover/Discard prompt on next launch.

## Production-readiness pass — P0/P1  ✅ DONE (audio-listening verify pending)

Stability / durability / audio-safety hardening from a deep audit (details in
`PRODUCTION_HANDOFF.md`). All committed on `master`, host + VM-on-target tested.
- **P0 (10/10)** — pre-DAC NaN/denormal guard; atomic project save + parse-into-
  temp load (a corrupt file never wipes the session); WavSource OOB + count-DoS
  caps; block-wise/leaf-scratch export (OOM); cross-track move data-loss guard;
  atomic `Bus` mix params (UpdateMix UI/RT race); monitor/live-MIDI teardown UAF.
- **P1** — bus-stem silence; note-length clamp; save precision + string escaping
  + strict versioned header; Freeverb input gain; SMF bounds; MIDI panic on stop;
  underrun resync; xrun take-length pad; marker identity by frame+name; channel-
  aware resampler; undo-history cap; WavWriter 4 GB guard + NaN sanitize; zero-
  rate reject. Fuzz corpora + NaN-injection + malformed-SMF regression tests.
- *Remaining P1 minors:* autosave-during-recording.
  (DONE: zipper-noise smoothing — ~10 ms coefficient glide in Biquad/Eq and
  makeup/range-floor glide in Compressor/Gate, so a stepped/automated parameter
  ramps instead of clicking. Host-tested `smoothing_tests`, ASan-clean.)
  (DONE: Eq FFT moved off the RT thread — RT captures + double-buffer-publishes
  each analyzer frame, the FFT runs lazily UI-side in `Eq::Spectrum()`.)

## Phase X — Export & mastering  ◐ IN PROGRESS

- ✅ Bit depth: 16-bit (TPDF-dithered) / 24-bit PCM / 32-bit float; `ExportWav`
  `bitDepth` param; fmt-chunk asserted in tests.
- ✅ Loudness normalization: BS.1770 integrated-LUFS target with a true-peak-safe
  dBTP ceiling (gain-based). Reuses `Loudness` offline.
- ✅ Look-ahead true-peak limiter (`dsp/Limiter`): offline, 4x-oversampled
  detection, stereo-linked, anticipatory attack (zero added latency) + release;
  guarantees the output dBTP ceiling by limiting peaks. Wired into `ExportWav`
  (`ExportNormalize::limiter`) so normalization can hit the target LUFS and the
  limiter — not a whole-program attenuation — holds the ceiling. Host-tested.
- *Remaining:* export bit-depth/rate/format **selection UI**; worker-thread
  export with progress/cancel; FLAC/Ogg/MP3 encoders (bundled/own libs — no
  Media Kit decoders on target); export presets + ranges.

## Phase Y — Latency compensation  ◐ IN PROGRESS

- ✅ Infrastructure: `IEffect::LatencySamples()` (default 0); kit-free PDC latency
  solver `model/Pdc.h` (per-node in/out latency + per-edge compensating delay
  over the routing DAG, reusing the routing edge model). Host-tested
  (`pdc_tests`).
- ✅ Offline exporter PDC: measures each node's fx-chain latency, delay-aligns
  every edge (output + sends + master), renders into padded buffers, then trims
  the leading latency so a bounce stays timeline-aligned (a latent plugin is made
  transparent, not shifted). Host-tested end-to-end with a synthetic-latency
  plugin (`exporter_pdc_tests`: impulse transparency + dry-sibling alignment).
  With no latent effect, `pad == 0` and the render is byte-identical to before.
- ✅ RT engine PDC: kit-free `engine/FrameDelay.h` stereo delay line
  (host-tested), one per routing edge (output + each send), sized from the PDC
  solve at Load. Every built-in effect reports 0 latency, so all delay lines are
  length 0 (a plain accumulate) and playback is bit-identical until a latent
  effect appears. Cross-compiles clean; **VM audio-runtime verification pending.**
- ✅ Record round-trip offset math: `RecordPlan::CompensateRoundTrip` slides a
  take earlier by the record round-trip latency (host-tested), plumbed into the
  take-placement path via `MainWindow::fRoundTripFrames` (0 until the device
  latency is queried — Media Kit, VM-gated).
- ✅ Live latent effect: `dsp/LookaheadLimiter` — the first built-in reporting a
  non-zero `IEffect::LatencySamples()`, so PDC is now exercised by a real
  built-in (before it, every delay line was length 0). Streaming look-ahead
  brickwall limiter (La-frame delay + sliding window-max gain, RT-safe); output
  lags input by exactly La frames. Host-tested (`lookahead_limiter_tests`) and
  `exporter_pdc_tests` gained a real-`LimiterDesc` transparency case. Engine +
  Exporter pick up its latency generically; addable from the effects editor.
  **VM audio-runtime verification pending.**
- *Remaining:* the Media-Kit device-latency query feeding `fRoundTripFrames`;
  round-trip compensation for the punch/loop-record paths; BBT/playhead report
  offset by the master latency during playback.

## Phase Z — MIDI depth  ◐ IN PROGRESS (channel controls done end to end)

- ✅ Event model (kit-free, host-tested): `MidiClipEvent` (CC / pitch-bend /
  program / channel-pressure) stored clip-relative on `MidiClip.events`;
  `Track::CollectEvents()` mirrors `CollectNotes` (absolute-timeline,
  take/window-aware). Distinct from the live-transport `daw::MidiEvent`.
- ✅ IO: `SmfIO` captures + writes CC/PB/PC/pressure (`SmfEvent`, variable
  data-byte count); `ProjectIO` `mev` lines; both round-trip tested. `MainWindow`
  SMF import/export carries events through; note edits preserve them
  (`SetMidiClipNotesCommand` touches only `notes`).
- ✅ Audible channel volume/expression: kit-free `MidiControl.h`
  (`CcValueAt` / `PitchBendAt` / `MidiChannelGain` = CC7 × CC11, host-tested).
  The Exporter (host-tested: CC7 mutes/halves/passes) and the Engine render MIDI
  notes through this per-block gain, so a `.mid`'s volume/expression plays back.
  Engine wiring cross-compiles; VM audio-runtime pending.
- ✅ Channel pan (CC10): `MidiChannelPan` + `MidiChannelGains` fold the CC7×CC11
  magnitude into per-side gains, and `Synth::Render` takes stereo gains so a
  voice is placed without a second pass. A BALANCE law (unity at center, only the
  far channel attenuates) — deliberately not equal-power, so a project with no
  CC10 is unchanged. Engine + Exporter wired; host-tested (`midicontrol_tests`
  plus an end-to-end bounce case: hard-left silences R, centered == no CC10).
- ✅ Per-block CC smoothing: `Synth::Render` takes a `StereoGain` pair and ramps
  linearly across the block, reaching the target on the last frame; the engine
  (per `Bus`) and the exporter carry the previous block's end gain forward, so a
  stepped CC7/CC10/CC11 glides instead of clicking at the block seam. `from ==
  to` is bit-identical to the old constant path, and the first block after a
  Load/seek snaps rather than sweeping from a stale value. Host-tested
  (`synth_tests` ramp/continuity + an end-to-end bounce asserting a CC7 step
  fades over a block — the test fails if the ramp is defeated).
- ✅ CC lanes in the piano roll: the bottom strip switches between note velocity
  and one continuous controller — only the ones the synth renders (CC7 Vol,
  CC11 Expr, CC10 Pan), so every edit here is audible. Click/drag paints points,
  right-click deletes; drawn as a *staircase* because `CcValueAt` is a step
  function, so the shape on screen is what the engine renders. Edits post
  `kMsgApplyEvents` -> `SetMidiClipEventsCommand`, which touches only
  `MidiClip::events` — the disjointness from `SetMidiClipNotesCommand` is
  host-tested (the test fails if either command clobbers the other's half).
  Like note edits, controllers are snapshotted at engine Load, so a change is
  heard on the next Start.
- *Remaining:* pitch-bend + mod-wheel (both modulate frequency, so the stateless
  synth needs phase integration — deferred); a generic "other CC" lane (today
  only the three audible controllers are offered); live-MIDI CC recording
  (`MidiPort`/`MidiRecorder`); per-track MIDI input demux; MIDI clock/MTC;
  sysex.

## Phase I — Plugins (long-term)

Internal effect add-on ABI (`PluginHost` + native `.so` exists), then LV2
hosting with native generic GUIs (ARCHITECTURE §8 stages 2–3). VST is a maybe.

## Ardour-parity gap (what the phases above do NOT yet cover)

Phases A–I give a capable small DAW, not Ardour parity. `ARCHITECTURE.md`
already declares non-goals (no Pro Tools/Ableton parity, no VST3 GUI at first,
no video/notation/cloud). Beyond the phases, a pro DAW like Ardour also has:

- **Plugins** — LADSPA/LV2/VST2/VST3/AU hosting, plugin delay compensation
  (PDC), presets, sidechain, plugin scan/manager. (We have built-in DSP only;
  hosting is Phase I.)
- **Latency compensation** — record + plugin PDC across the graph.
- **Advanced routing** — VCA master faders, a monitor/control-room section
  (dim/mono/AFL/PFL), an any-to-any routing matrix, external hardware inserts,
  full sidechain routing.
- **Metering** — K-system, phase/correlation, a meterbridge. (We have per-track
  peak meters + a BS.1770 master LUFS / true-peak readout; K-system and
  correlation are the gaps.)
- **Audio editing** — time-stretch / pitch-shift (Rubber Band), transient
  detection, audio quantize, strip-silence, region normalize/reverse/gain,
  ripple + slip/slide edit, playlists per track.
- **Tempo/time** — a real tempo/meter MAP (multiple changes + ramps), BBT,
  timecode. (We have a single project tempo.)
- **Sync** — MTC / LTC / MMC, JACK transport, Ableton Link, master/slave.
- **Control surfaces** — Mackie/OSC/generic MIDI-learn.
- **MIDI depth** — SMF import/export, CC/controller lanes, step entry, program
  change/sysex, MIDI clock. (We have an internal piano roll only.)
- **Comping / cue** — take comping, a clip-launch (cue) page.
- **Track ops** — freeze/bounce a track, track templates, folder tracks.
- **Export** — multi-format (FLAC/MP3/Ogg), export presets, ranges. (We have
  stems + per-track/per-bus bounce, 16/24/32-bit with TPDF dither, BS.1770
  loudness normalization, and a look-ahead true-peak limiter — Phase X;
  compressed formats + presets + ranges are the gaps.)
- **Session** — snapshots, templates, archive/bundle, cleanup-unused, autosave,
  markers / ranges / locations.
- **Other** — video timeline, Lua scripting, surround/VBAP panning, spectral
  analysis/spectrogram.

Reaching full Ardour parity is a multi-year effort. The realistic target for
this project is a **clean, native, genuinely useful Haiku DAW** — the phases
above — with parity items pulled in selectively where they matter and Haiku
makes them idiomatic (e.g. BFS browser, native metering). Some Ardour features
are out of scope by design (video, heavy plugin ecosystems on a plugin-less
Haiku image, control-surface zoo).

### Recommended additions to the plan (high value + feasible here)

These are the Ardour-tier gaps worth building — mostly kit-free / host-testable,
native-feeling, and not blocked by the plugin-less VM:

- **Tempo & meter MAP** *(new, high priority)* — multiple tempo + time-signature
  changes with ramps, replacing the single project tempo. Kit-free extension of
  `Grid`; touches the ruler, snap, metronome, synth, and export. Foundational
  for real arrangements. → new **Phase T**, slot right after Phase A.
- **Markers / ranges / locations** *(easy, high workflow value)* — named
  markers, loop/punch ranges, a locations list. Kit-free model + UI. → fold into
  Phase C.
- **Metering upgrade** — per-track meters, and LUFS + true-peak on the master
  (kit-free loudness math, host-testable). → extend Phase A / Phase G.
- **Export upgrade** — stem / per-track / per-bus bounce, export ranges,
  dithering, loudness-normalize; FLAC/Ogg encoders later. → extend the exporter.
- ~~**Freeze / bounce a track**~~ ✅ done (Phase C8).
- **Region ops** — ✅ normalize / reverse / strip-silence / clear-fades done
  (Phase C7); gain (Ctrl-drag) already existed. *Later:* fade presets, region
  reverse-with-gain, per-clip normalize target level.
- **SMF (MIDI file) import/export + CC lanes** — standard `.mid` read/write
  (kit-free) and controller lanes that feed automation. → Phase B / E.
- **Snapshots + autosave** — cheap given `ProjectIO`. → Phase H.
- **Basic monitor section** — master dim / mono / solo-safe. Small. → Phase A.
- **Time-stretch / pitch-shift** *(stretch — harder)* — a basic WSOLA/phase-
  vocoder, kit-free. High value, high effort; flag as its own later phase.

### Deliberately skipped (for now — low value here or out of scope)

Full plugin hosting (LV2/VST) & PDC beyond Phase I; timecode/LTC/MTC/MMC &
Ableton Link sync; control surfaces / OSC / MIDI-learn; VCA masters + full
routing matrix (basic buses/sends in Phase A are enough for now); transient
detection / audio quantize; video timeline; Lua scripting; surround/VBAP. These
can be reconsidered once the core is done and if a concrete need appears.

---

### Suggested order

A (routing) → B (automation) → C (editing) → D (recording) → G (a polish pass) →
F (browser) → E2/D2 (external + MIDI recording, once MIDI is testable) → I
(plugins). E1 (better synth) and H (persistence) can slot in opportunistically.
