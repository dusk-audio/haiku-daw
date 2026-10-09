# Haiku DAW

A native digital audio workstation for [Haiku](https://www.haiku-os.org/):
multitrack audio and MIDI, mixing with buses and sends, automation, built-in
and LV2 effects, and offline export — built on the Media Kit, the Midi Kit and
the Interface Kit, not ported from somewhere else.

## What it does

- **Timeline editing** — clips with drag/move/trim, fades and crossfades,
  split, multi-select, per-clip gain, loop and punch regions, markers, and a
  tempo/meter map (including ramps) that the grid, the metronome and the
  piano roll all follow.
- **Recording** — audio takes with count-in, overdub, punch-in/out and
  loop-record with take comping; input monitoring; round-trip latency
  compensated from what the Media Kit reports.
- **MIDI** — a piano roll with a velocity and CC lane, an internal synth and an
  SFZ/SF2 sampler per track, SMF import and export, external MIDI input with
  per-track assignment, and a transform suite (quantize with strength, swing
  and triplets, humanize, legato, transpose, velocity).
- **Mixing** — per-track fader/pan/mute/solo, buses and aux sends with pre/post
  fader taps, master bus effects, monitor dim/mono, per-track meters and a
  BS.1770 loudness meter (momentary, short-term, true peak).
- **Automation** — volume, pan and any effect parameter, drawn on the timeline.
- **Inserts** — ten built-in effects (EQ with spectrum, compressor, gate,
  reverb, delay, saturator, widener, look-ahead limiter, …), native add-ons,
  and **LV2** plugins with their own editors when the plugin ships one.
- **Export** — mixdown or stems, 16/24-bit PCM or 32-bit float, TPDF dither,
  loudness normalization to a target LUFS with a true-peak ceiling, and a
  look-ahead limiter; renders on a worker thread with a progress bar you can
  cancel.
- **Project handling** — one text `.dawproj` file with portable relative media
  paths, autosave with crash recovery, and a sample browser over BFS
  attributes and live queries.

## Requirements

- **Haiku R1/beta5 or newer** (developed on R1/beta6). The app uses the Media,
  Midi Kit 2, Interface, Storage and Tracker kits.
- **CMake 3.16+** and a C++17 compiler (`pkgman install cmake gcc`).
- **Optional: LV2 hosting** — install `lilv`, `lilv_devel` and `lv2`
  (`pkgman install lilv lilv_devel lv2`). Without them the build simply has no
  LV2 support; everything else works, and the build says so.
- **Audio** — any device the Media Kit drives. The HDA driver is what this has
  been tested on.

## Building

```sh
cmake -S . -B build
cmake --build build -j4
./build/daw
```

`cmake -B build -DDAW_LV2=OFF` builds without LV2 (the configuration CI-style
checks use). `cmake -B build -DDAW_SANITIZE=ON` builds with ASan/UBSan for the
test suite.

Run the tests with `ctest --test-dir build`. The suite is almost entirely
kit-free and runs anywhere; `ui_functional_tests` needs a display and skips
(exit 77) without one.

## Known limitations

- **Compressed audio is not supported** — WAV in, WAV out. FLAC/Ogg/MP3 need
  bundled encoders (the target image has no Media Kit decoder plugins), and
  they are not in 1.0.
- **A plugin whose reported latency changes while it runs** cannot be
  compensated exactly: the host latches the value when the plugin is
  activated, so PDC can be off by the amount it moved (a few samples for the
  plugin this was measured with).
- **VST is not supported**, by design: LV2 is the plugin format here.
- The **VM** this is developed against has no usable audio; sound is verified
  on real hardware.
- **No video**, no notation, no cloud — those are non-goals, not gaps.

## License

MIT — see [LICENSE](LICENSE).
