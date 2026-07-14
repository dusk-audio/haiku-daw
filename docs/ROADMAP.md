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
- **B3.** ✅ Header "Auto" box (Off/Gain/Pan); the lane content area draws the
  curve and edits breakpoints (click add / drag move / right-click delete),
  one undoable gesture per drag.
- *Deferred:* per-effect-param + send-level lanes (model generalizes cleanly).

## Phase C — Editing depth  ✅ DONE

- ✅ Per-clip gain (C1): `Clip.gain`, engine+Exporter apply, IO, Ctrl-drag +
  dB label.
- ✅ Clip split (C2): `SplitClipCommand`, right-click "Split here".
- ✅ Track reorder (C3): `MoveTrackCommand`, name menu Move Up/Down.
- ✅ Per-track color + height (C4): 6-color palette + variable lane height
  (LaneRect sums heights); name menu Next Color / Taller / Shorter.
- ✅ Crossfade (C5): kit-free `ComputeCrossfades` — overlapping clips auto
  fade-out/in; engine + Exporter derive effective fades. Host-tested.
- ✅ Multi-select + range ops (C6): clip selection (click / Shift-click /
  rubber-band / Esc), group move, Delete, Ctrl-D duplicate — each one undo step
  via `MacroCommand`. (MIDI-note multi-select deferred.)

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
- *Deferred:* sample/wavetable synth; band-limited oscillators (naive now).
- **External Midi Kit 2 I/O** (keyboards, hardware synths) — *deferred until the
  VM can pass through MIDI; `scripts/midi_probe.sh` is ready to characterize it.*

## Phase F — Sample browser  ✅ DONE

- ✅ BFS attribute tagging: `src/storage/BfsAttr` reads/writes DAW:bpm / DAW:key
  / DAW:duration + creates their fs indexes; import auto-tags duration.
- ✅ Live `BQuery` browser (`SampleBrowser`, View > Sample Browser): name filter
  over the boot volume, lists audio files + their duration/BPM, double-click
  imports, BPM field tags the selection. Compile-checked + VM-built.
- *Later:* drag-and-drop into the timeline; BPM/key range predicates; Tracker
  MIME attr registration so columns show there too.

## Phase G — UI / UX polish pass  ✅ DONE (first pass)

- ✅ Vertical track scroll (mouse wheel + PageUp/Down), clamped to content.
- ✅ Zoom to Fit (F / View menu).
- ✅ BBT transport readout (bar.beat + min:sec, tempo-map).
- ✅ Keyboard Shortcuts help (View menu).
- *Later:* styled widgets/theming, per-track meters, tidy dialogs, drag-resize
  panels. Revisit after more features land.

## Phase H — Persistence & robustness

- Save app settings (buffer size, last dir) and window layout.
- Project bundle: keep recorded takes next to the `.dawproj`.
- Autosave / crash recovery.

## Phase I — Plugins (long-term)

Internal effect add-on ABI, then LV2 hosting with native generic GUIs
(ARCHITECTURE §8 stages 2–3). VST is a maybe.

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
- **Metering** — K-system, true-peak, LUFS/loudness, phase/correlation, a
  meterbridge. (We have simple master peak only.)
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
- **Export** — stems, multi-format (FLAC/MP3/Ogg), dithering, loudness
  normalization, export presets, ranges. (We have one 16-bit WAV bounce.)
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
- **Freeze / bounce a track** — render a track through its FX to audio (reuse
  the offline `Exporter`). → Phase C.
- **Region ops** — normalize / reverse / gain / strip-silence / fade presets on
  a clip (kit-free DSP, host-testable). → Phase C.
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
