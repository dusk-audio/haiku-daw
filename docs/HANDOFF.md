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
- **M4 timeline UI** ⬅ **NEXT** — first `BWindow`/`BView`: draw clips +
  waveforms, transport bar, playhead, per-track mute/solo/gain/pan.
- M5 recording, M6 MIDI, M7 DSP effects — after M4.

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
src/engine/
  WavSource.{h,cpp}    native RIFF/WAVE reader (kit-free, host-testable)
  RingBuffer.h         lock-free SPSC float ring
  Engine.{h,cpp}       BSoundPlayer output + TrackStream mixing (Haiku-only)
tests/                 model_tests, wav_tests  (host-buildable)
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

## Next task — M4 timeline UI

Build the first window with the Interface Kit:
- `BWindow` + a custom timeline `BView` (`Draw()`, mouse handlers).
- Render clips as blocks; draw **waveforms from a precomputed peak cache**
  (min/max per pixel column, built on import — never scan full audio on
  redraw).
- Transport bar: play/stop, playhead readout, tempo.
- Track headers: name, mute/solo/arm, gain fader, pan.
- Meters fed from the engine via a lock-free level queue (UI polls ~30–60 Hz
  with `BMessageRunner`).
- Wire the existing `CommandStack` to UI actions (drag clip → `MoveClipCommand`,
  etc.). The engine already honors solo/mute/gain/pan.

Study Haiku's **Cortex** (Media Kit node-graph editor) and **MediaPlayer**
in the Haiku source tree for Interface Kit + Media Kit patterns.

Keep every milestone runnable; keep the audio thread real-time-safe.
