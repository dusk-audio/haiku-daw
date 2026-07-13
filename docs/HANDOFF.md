# Handoff — Haiku DAW

Read this first, then `docs/ARCHITECTURE.md`. This is a working project with
a running audio engine; you are continuing it, not starting it.

## What this is

A native **digital audio workstation for Haiku OS**, C++, built on the
Haiku **Media Kit** (audio I/O), **Midi Kit 2** (planned), and **Interface
Kit** (UI, not yet started). Owner: Marc. The full design of record is
`docs/ARCHITECTURE.md`; the milestone ladder is there too.

## Current status (milestones)

- **M0 hello_beep** ✅ — `BSoundPlayer` sine, proved toolchain + audio out.
- **M1 model + undo/redo** ✅ — `src/model/`, kit-free, host-tested.
- **M2 single-clip playback** ✅ — engine decodes WAV, streams from disk,
  plays through `BSoundPlayer`.
- **M3 multitrack mix** ✅ — solo + equal-power pan, sums N tracks.
  Confirmed working on real Haiku (tracks summed + panned correctly).
- **M4 timeline UI** ✅ — first `BWindow`/`BView`, confirmed on real Haiku.
  Timeline with ruler, per-track lanes, clip blocks + waveforms (from a
  `PeakCache`), transport bar (play/stop + time), sweeping playhead, master
  stereo meter, track headers (name, mute/solo, drag gain/pan), clip drag,
  undo/redo, and click-to-seek. See "M4 state" below.
- **M5 recording** ✅ — capture the system audio input to a WAV take and
  drop it as a clip. `BMediaRecorder` (recipe: `GetAudioInput` +
  `Connect(node, NULL, wildcard)`; device negotiates native format — 96 kHz
  int16 stereo on the VM's HD Audio) → RT record hook → lock-free ring →
  disk-writer thread → `WavWriter`. UI: arm (R) + Rec, take dropped via
  `AddClipCommand` at the playhead. Proven end-to-end on real Haiku.
- **Resampler** ✅ — streaming linear SRC per stream on its disk thread, so
  the RT mixer stays 1:1 and any source rate plays at correct pitch (44.1 k /
  96 k, incl. recorded takes). Clip lengths stored in timeline frames; source
  seek + waveform mapping are rate-corrected. Confirmed on real Haiku.
- **M7 DSP effects** ✅ — `IEffect` (Biquad, Delay) + `EffectFactory`; model
  Track carries an ordered `EffectDesc` chain (Add/ClearEffects commands);
  engine mixes each track's clips into a bus, runs the chain, sums to master.
  UI: FX box opens a per-track effects editor (add/remove + param sliders).
  Confirmed audible on real Haiku. (Chain is per-track post-mix; effect
  reordering is the only editor gap left.)
- **M6 MIDI** ✅ (internal-synth path) — `Track` carries a `MidiNote` list
  (`AddNoteCommand`); kit-free `Synth` (polyphonic sine, AR envelope,
  deterministic phase) renders notes; engine adds a bus per audible MIDI track
  and renders through the same FX chain + master mix + transport clock as
  audio. UI draws a simple piano roll; notes can be added, moved, resized, and
  velocity-edited (see the editing entry). Confirmed on real Haiku. (External
  Midi Kit 2 I/O still to do.)
- **M8 project save/load** ✅ — kit-free `ProjectIO` text serializer over the
  whole model (round-trip host-tested), wired to Save/Open `BFilePanel` +
  Cmd-S/Cmd-O; load clears history, rebuilds peaks, refreshes the timeline.
  Confirmed on real Haiku. Media paths are stored **relative to the project
  file** and resolved on load, so a project + co-located media are portable.
  (Bundling recorded takes next to the `.dawproj` is a later refinement.)
- **Editing + musical grid + loop** ✅ — delete/move/resize notes+clips,
  velocity (Ctrl-drag), clip fades (drag top corners), snap to 16ths (Shift =
  free), bar/beat ruler, loop (drag ruler), zoom (+/- , arrows pan), master
  volume, per-track effects editor window. Portable relative media paths.
- **Big batch (agents + integration)** ✅ (host-tested where kit-free):
  - **Track management** — menu New Audio/MIDI Track, right-click track name to
    delete, double-click to rename (RenameWindow). Commands: RemoveTrack,
    SetTrackName.
  - **Clip fades** — `SetClipFadeCommand`; engine `TrackStream::Mix` +
    Exporter apply linear fade-in/out; drag a clip's top corners to set them.
  - **Offline export** — kit-free `ExportWav(project, path, outRate)` bounces
    the whole project to a 16-bit WAV (File ▸ Export WAV). Host-tested.
  - **Mixer window** — channel strips (gain/pan/M/S), snapshot+post pattern.
  - **New DSP effects** — Reverb + Compressor (ported from the user's
    ~/projects/plugins), wired into EffectFactory/EffectsWindow. Host-tested.
  - **Menu bar** — File (Open/Save/Export/Quit), Edit (Undo/Redo), Track,
    View (Mixer); Save/Open/Undo/Redo buttons removed from the transport bar.
  - **MIDI probe** — `scripts/midi_probe.sh` (run on VM before external MIDI).
- **NEXT** — external Midi Kit 2 I/O (after running midi_probe.sh), automation,
  BFS sample browser, copy/paste, metronome.

## Architecture in one breath

Model (`src/model/`) is the source of truth: `Project` → `Track`s →
`Clip`s, mutated only through a **command stack** (undo/redo). The engine
(`src/engine/`) plays it: one `BSoundPlayer` output, one **`TrackStream`**
per clip (each owns a `WavSource` + a lock-free SPSC **`RingBuffer`** filled
by its own disk thread), and the real-time `BSoundPlayer` callback **sums**
all streams applying per-channel gain/pan.

**Threading rule (do not break):** the audio callback does no allocation,
no locks, no file I/O — only arithmetic on ring buffers + atomics. Disk
threads do all file reading. UI (when it exists) mutates the model on its
own thread and hands intents to the engine; it never writes engine memory.

## Repo layout

```
docs/ARCHITECTURE.md   design of record (read after this)
docs/HANDOFF.md        this file
CMakeLists.txt         model+tests build on ANY host; engine/UI gated if(HAIKU)
src/model/             Project/Track/Clip, Command stack, commands  (kit-free)
  PeakCache.{h,cpp}    min/max waveform envelope (kit-free, host-testable)
src/engine/
  WavSource.{h,cpp}    native RIFF/WAVE reader + Seek (kit-free, host-testable)
  WavWriter.{h,cpp}    native RIFF/WAVE writer for takes (kit-free, host-test)
  Resampler.{h,cpp}    streaming linear SRC (kit-free, host-testable)
  RingBuffer.h         lock-free SPSC float ring
  Engine.{h,cpp}       output + per-track bus mixing + SRC + FX (Haiku-only)
  Recorder.{h,cpp}     BMediaRecorder capture -> ring -> WavWriter (Haiku-only)
src/dsp/               kit-free, host-testable DSP
  IEffect.h            effect interface (Prepare/Process/Reset)
  Biquad.{h,cpp}       RBJ low/high/peaking filter
  Delay.{h,cpp}        feedback delay
  EffectFactory.{h,cpp} EffectDesc -> IEffect
src/ui/                Interface Kit (Haiku-only): App in main.cpp
  UiMetrics.h          layout constants, palette, kMsgSeek
  MainWindow.{h,cpp}   BWindow: transport bar, engine ownership, playhead poll
  TimelineView.{h,cpp} custom BView: ruler, lanes, clips, waveforms, headers
  MeterView.{h,cpp}    stereo master level meter
src/main.cpp           BApplication; seeds a Project from argv WAVs (Haiku-only)
src/model/Effect.h     kit-free EffectDesc (serializable effect params)
src/model/Grid.h       kit-free bars/beats math + snap (header-only)
src/model/ProjectIO.*  kit-free text save/load (portable relative media paths)
tests/                 model_tests, wav_tests, peak_tests, wavwriter_tests,
                       resampler_tests, effect_tests, synth_tests,
                       projectio_tests, grid_tests  (all host-buildable)
prototypes/record_clip/ M5 driver: capture N seconds to a WAV
prototypes/
  hello_beep/          M0
  play_clip/           M2 driver: play one WAV
  mix_project/         M3 driver: mix N WAVs, path[@gain[:pan]]
scripts/               VM helper scripts (git-pulled + run in the VM)
```

## CRITICAL constraint — this Haiku image has no media reader plugins

The target VM (Haiku **hrev57937**, r1beta5-era, x86_64) ships Media Kit
*nodes* but **no file reader/decoder plugins** (`/boot/system/add-ons/media/
plugins/` does not exist). So `BMediaFile` returns **"No handler"** and
MediaPlayer itself can't open a WAV. `pkgman install ffmpeg` does NOT fix it
(that ffmpeg plugin is Qt's, not Media Kit's).

**Consequence:** we do our own file I/O. `WavSource` is a hand-written
RIFF/WAVE parser feeding `BSoundPlayer` (which needs no plugins and works).
Do **not** reintroduce `BMediaFile`. Future compressed formats (mp3/flac)
will need our own parser or a bundled decode library, not the Media Kit.

## Build & test

**On any host (Linux/macOS — fast iteration, no VM):** the model layer and
`WavSource` are kit-free.
```
cmake -B build && cmake --build build
./build/model_tests    # 27 checks
./build/wav_tests      # 11 checks
```
Engine + prototypes are skipped off-Haiku (`if(HAIKU)` in CMake). Always
run these before handing code to the VM.

**On Haiku (the VM):** full build incl. engine + drivers.
```
cmake -B build && cmake --build build
./build/play_clip  <file.wav>
./build/mix_project <file.wav@gain:pan> ...
```

## Dev loop (host ⇄ VM)

Claude edits + builds/tests kit-free code on Marc's **Linux host**. Haiku
runs in a **GNOME Boxes / libvirt KVM** VM. Code moves host→VM via git:
the host serves the repo over `python3 -m http.server 8000` (host IP
`192.168.1.230`); the VM `git pull`s from `http://192.168.1.230:8000/.git`.
There is **no GitHub remote**. To ship code: commit on host →
`git update-server-info` → in VM `git pull && cmake --build build`.

Helper scripts pattern: commit a script under `scripts/`, then in the VM
`cd ~/haiku-daw && git pull && sh scripts/<name>.sh`.

## Hard-won lessons (don't repeat these)

- **Do NOT touch the VM's libvirt network config.** Attempts at passt and
  bridged networking and qemu `custom-argv` each broke Haiku's boot or made
  gnome-boxes unable to open the VM. Plain user-NAT + the git-pull loop is
  the working setup. SSH-into-the-VM was abandoned on purpose.
- The VM decode failure was the missing media plugins (above), not the
  code — verified by MediaPlayer also failing and by `media_probe.sh`.

## Commit rules

- Author **`marc@duskaudio.com`** (audio project) or `marc@lunacoap.com`.
- **Never** add a `Co-Authored-By:` trailer. No AI attribution in history.
- Conventional-commit style subjects (`feat(engine): ...`, `fix(...)`).

## M4 state (how the UI is built — read before touching it)

- **One window, no menu bar.** `MainWindow` stacks a transport `BView` strip
  (Play/Stop, `mm:ss.mmm` readout, Undo/Redo buttons, `MeterView`) over a
  `TimelineView` that fills the rest.
- **TimelineView is fully custom-drawn** (no child `BControl`s). Left gutter
  (`kHeaderWidth`) is the per-track header (name, M/S boxes, gain fader, pan
  bar); right of it is time content (ruler + lanes + clip blocks + waveforms
  + playhead). Frame⇄pixel via `FrameToX`/`XToFrame`; `kDefaultFramesPerPixel`
  zoom. Geometry lives in `UiMetrics.h`.
- **All model edits go through `CommandStack`.** Header clicks build
  `SetTrackMute/Solo/Gain/Pan` and clip drags build `MoveClipCommand`.
  Fader/clip **drags preview by writing the model directly**, then on
  `MouseUp` restore the pre-drag value and push ONE command → a drag is a
  single clean undo. `SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS)`
  grabs the pointer for the drag.
- **Playback = rebuild-on-play.** `StartPlayback` makes a fresh `Engine`,
  `Load`s the model at `transport.playhead`, `Start`s. A ~60 Hz
  `BMessageRunner` (`MSG_PULSE`) polls `Engine::Playhead()`/`PeakL/R()` and
  each tick calls `Engine::UpdateMix(model)` so **gain/pan/mute/solo are
  applied live** (atomics on `TrackStream`, no replay). Seek = click ruler →
  `kMsgSeek` → restart `Load` at the new frame (per-clip source pre-seek via
  `WavSource::Seek` aligns audio).
- **`PeakCache`** (kit-free) is built once per source in `main` and drawn as
  min/max per pixel column — never scans audio on redraw.

### GOTCHA: `Frame` vs `BView::Frame()` / `BWindow::Frame()`
`daw::Frame` (int64) is shadowed inside any `BView`/`BWindow` subclass by the
inherited `Frame()` method, so unqualified `Frame` fails to name a type. Each
such class declares `using Frame = daw::Frame;` to hide the inherited name.
Do the same in any new view/window that uses model frames.

## Next task — the full ladder (M0–M8 + resampler + DSP) is done

Editing (delete/move/resize/velocity/fades), the effects editor, portable
relative media paths, offline export, the mixer, and track management are all
**done** (see the status list above). Remaining work is reach, not core:

**A. External Midi Kit 2 I/O.** `BMidiRoster` / `BMidiLocalProducer/Consumer`
for hardware keyboards + ports and recording MIDI. Run `scripts/midi_probe.sh`
on the VM first (mirrors `record_probe.sh`).

**B. Automation.** Gain/pan/effect-param lanes over time.

**C. BFS sample browser.** Tag audio with BPM/Key/Duration attributes; a live
`BQuery` becomes an instant sample browser (ARCHITECTURE §5.2) — the native
Haiku superpower.

**D. Smaller items.** Copy/paste of clips + notes, and bundling recorded takes
next to the `.dawproj`. (Effect reordering, a metronome click, and MIDI
equal-power pan in both the live engine and export are done.)

Keep every milestone runnable; keep the audio thread real-time-safe; commit as
`marc@duskaudio.com` with no AI trailer; ship via the git-pull loop.
