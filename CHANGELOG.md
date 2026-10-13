# Changelog

## 1.0.0 — unreleased

The first release. Everything below is in it; the project's history before this
file existed is in `git log` and `docs/ROADMAP.md`.

**Editing**
- Timeline: clips with drag, move, trim, split, slip-edit and nudge, fades and
  automatic crossfades; multi-select with group move/delete/duplicate;
  per-clip name, gain, colour and mute; region ops (normalize, reverse, strip
  silence, clear fades); per-track colour and lane height; markers and a
  marker lane; zoom to fit.
- A time range independent of the clips, with Cut, Copy, Paste, Delete and
  Insert Silence; the clipboard holds many clips and pastes at the playhead.
- Time-stretch: Cmd-drag (or Alt-drag) a clip's right edge to stretch it to
  fit, with the ratio drawn on the clip. WSOLA on the disk thread and the same
  stretcher in the exporter, so a stretched clip plays and bounces the same and
  a seek lands on the same synthesis grid. Pitch stays as recorded — pitch-shift
  is not in this release.
- Region operations and track freeze run as background jobs with a progress bar
  and Cancel; the window stays responsive, a cancelled job applies nothing, and
  nothing half-written is left where a finished file is expected.
- Tempo and meter map with ramps; the grid, ruler, snap and metronome follow it.
- Undo/redo for every edit, with the command stack as the only way the model
  changes.

**Recording**
- Audio takes at the device's native rate, resampled to the project: count-in,
  overdub, punch-in/out, loop-record with take comping, input monitoring.
- One take per armed track: mono or stereo tracks, each with an input device
  and channel pair chosen from the inputs the system reports (by name, so the
  assignment survives a device renumbering); takes written as 16-bit, 24-bit or
  32-bit float.
- Round-trip latency compensated from the Media Kit's reported device latency.
- Autosave with crash recovery; recorded takes bundled next to the project;
  a disk-full write stops the take and keeps what was written.

**MIDI**
- Piano roll with velocity and CC lanes, an internal synth and an SFZ/SF2
  sampler per track, SMF import/export, external MIDI input with per-track
  assignment and channel demux; any controller number can be drawn in the lane.
- Sustain (CC64), pitch-bend and mod-wheel are audible: a sustaining pedal
  latches notes, and bend and wheel modulate the synth and the sampler.
- Transforms: quantize (grid, strength, swing, note ends), humanize, legato,
  transpose, velocity.

**Mixing and routing**
- Fader, pan, mute, solo, mute groups, solo groups, solo-safe; input trim and
  phase invert per track; buses; aux sends with pre/post fader taps; master bus
  effects; monitor dim and mono; per-track meters; a BS.1770 loudness meter
  (momentary, short-term, true peak).
- Mixer strips: send-level knobs, the trim knob with its dB value, the phase
  button, the solo-group box, and dB entry by double-clicking a fader.
- Automation for volume, pan and every effect parameter.
- Plugin delay compensation, offline and in the engine — including a plugin
  whose reported latency changes while it runs, which is observed, re-latched
  and re-solved without stopping playback (the brief re-alignment step is the
  documented cost).

**Effects and plugins**
- Nine built-in effects: EQ with spectrum, biquad filter, compressor, gate,
  reverb, delay, saturator, stereo widener, look-ahead limiter — plus the
  native add-on ABI.
- Sidechain: the compressor and the gate take a key from any other track, and
  an LV2 plugin with sidechain inputs gets the engine's key pair.
- LV2 hosting: insert slots with bypass and wet/dry, a plugin browser, plugin
  state saved into the project, presets from the insert slot, and the plugin's
  own editor when it ships one — driving the playing insert.

**Export**
- Mixdown or stems; WAV at 16/24-bit PCM or 32-bit float; FLAC at 16/24-bit;
  Ogg Vorbis with a quality setting; TPDF dither for 16-bit; loudness
  normalization to a target LUFS with a true-peak ceiling and an optional
  look-ahead limiter; whole project or loop range.
- Import of WAV, AIFF/AIFC, FLAC and Ogg Vorbis, sniffed from the file's header
  rather than its extension.
- Export at another sample rate resamples through a windowed-sinc filter
  (Kaiser, 192 taps): aliasing and images are more than 100 dB down, and the
  passband is flat to a hundredth of a dB.
- Renders on a worker thread with a progress bar and cancel; a cancelled
  export leaves nothing behind.

**Application**
- A transport bar with the full button row (each showing its state), a large
  time display that cycles bars:beats, min:sec, samples and timecode and takes
  a typed position, tempo/meter, a DSP-load peak hold with an xrun counter, and
  a status line.
- A Preferences window (buffer size, default sample rate, count-in, metronome
  sound and level, monitor input, dark mode, UI scale, default project folder,
  autosave interval) and Project Settings (sample rate with an optional
  resample of the media, tempo, meter, notes). Output-device selection is not
  in this release: the Preferences row for it is disabled and audio goes to the
  system default device.
- File ▸ New opens a template chooser — Empty, Band (eight audio tracks into a
  bus) and Producer (MIDI and audio with a reverb bus) — from `.dawproj`
  templates the package installs.
- A single `.dawproj` text file with portable relative media paths; preferences
  and window layout persisted; a sample browser over BFS attributes and live
  queries; keyboard shortcuts help.
- A single-instance application with a registered `.dawproj` type: Tracker
  double-click, `open`, drag-onto-the-icon and a path on the command line all
  reach the running instance.
- Native Haiku look by default, or the DAW's own dark palette applied to its
  own windows (the Preferences window's Dark mode). The app never changes the
  operating system's appearance.
- Requires Haiku R1/beta5 or newer. LV2, FLAC and Ogg Vorbis support are
  optional at build time and all three are in the packaged build.
