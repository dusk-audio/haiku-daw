# Changelog

## 1.0.0 — 2026-10-09

The first release. Everything below is in it; the project's history before this
file existed is in `git log` and `docs/ROADMAP.md`.

**Editing**
- Timeline: clips with drag, move, trim, split, fades and automatic
  crossfades; multi-select with group move/delete/duplicate; per-clip gain;
  region ops (normalize, reverse, strip silence); per-track color and lane
  height; markers; zoom to fit.
- Tempo and meter map with ramps; the grid, ruler, snap and metronome follow it.
- Undo/redo for every edit, with the command stack as the only way the model
  changes.

**Recording**
- Audio takes at the device's native rate, resampled to the project: count-in,
  overdub, punch-in/out, loop-record with take comping, input monitoring.
- Round-trip latency compensated from the Media Kit's reported device latency.
- Autosave with crash recovery; recorded takes bundled next to the project.

**MIDI**
- Piano roll with velocity and CC lanes, an internal synth and an SFZ/SF2
  sampler per track, SMF import/export, external MIDI input with per-track
  assignment and channel demux.
- Transforms: quantize (grid, strength, swing, note ends), humanize, legato,
  transpose, velocity.

**Mixing and routing**
- Fader, pan, mute, solo, mute groups, solo-safe; buses; aux sends with pre/post
  fader taps; master bus effects; monitor dim and mono; per-track meters; a
  BS.1770 loudness meter (momentary, short-term, true peak).
- Automation for volume, pan and every effect parameter.
- Plugin delay compensation, offline and in the engine.

**Effects and plugins**
- Nine built-in effects: EQ with spectrum, biquad filter, compressor, gate,
  reverb, delay, saturator, stereo widener, look-ahead limiter — plus the
  native add-on ABI.
- LV2 hosting: insert slots with bypass and wet/dry, a plugin browser, and the
  plugin's own editor when it ships one — driving the playing insert.

**Export**
- Mixdown or stems; WAV at 16/24-bit PCM or 32-bit float; TPDF dither for
  16-bit; loudness normalization to a target LUFS with a true-peak ceiling and
  an optional look-ahead limiter; whole project or loop range.
- Renders on a worker thread with a progress bar and cancel; a cancelled
  export leaves nothing behind.

**Application**
- A single `.dawproj` text file with portable relative media paths; preferences
  and window layout persisted; a sample browser over BFS attributes and live
  queries; keyboard shortcuts help.
- Requires Haiku R1/beta5 or newer. LV2 support is optional at build time.
