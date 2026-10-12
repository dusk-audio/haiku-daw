# PR: Multichannel audio input (M5)

Branch `feature/multichannel-input`, spec `44-multichannel-input.md`.
Source of truth: `docs/PLAN_1.0.md` §M5.

## Base correction

The worktree was first created on `origin/docs/1.0-decisions` (`785cd5a`), 364
commits behind `master`: `src/ui/TrackInputs.h`, the current `Recorder.*`,
`docs/PLAN_1.0.md` and `docs/UI_GUIDELINES.md` as the brief referenced them do
not exist there. The coordinator approved a base fix; the branch was reset to
`master` (`85b19b1`) before any code was written, with zero commits to lose.
Everything below is on `85b19b1`.

## What was built

**1. Model — `Track.channels` and `InputSource {device, channelL, channelR}`**

- `InputSource` appends `std::string device` (the Media Kit input node's NAME,
  matched like a MIDI endpoint so the assignment survives node-id reshuffling;
  `""` = system default) and `int channelL`/`channelR` (0-based channels ON that
  device). `channel` keeps its MIDI-filter meaning; the appends are at the end
  of the struct so `InputSource{kind, name, channel}` still compiles.
- `Track` appends `TrackChannels channels` (`Mono`/`Stereo`, default `Stereo` =
  every earlier project's behaviour), at the end of the data members.
  `SetTrackChannelsCommand` makes the width an undoable document edit.
- Mono plays as a mono source with **no engine change**: `WavSource` already
  fans a 1-channel file out to L/R, and `EqualPowerGains` is the constant-power
  (-3 dB centre) mono law. What M5 adds is that a mono take is now *written*
  1-channel, from one device channel.

**2. Kit-free capture math — `src/model/ChannelSplit.h`** (host-tested)

`TakeFormat` (16-bit / 24-bit / float32) + `TakeFormatFromInt`/`TakeFormatBits`,
`TakeSpec` (path, mono|stereo, srcL/srcR, format), `PlanTake` (a track's address
and width -> a spec), `ClampToDevice` (the address against the channels the
capture actually negotiated), `Deinterleave` (interleaved device block -> one
take's mono/stereo block; an out-of-range channel reads silence, a stereo
address with no right channel duplicates the left). `NextFreeWavPaths`
(`TakeNames.h`) picks a whole capture's names in one pass, because the first
pick's file does not exist yet.

**3. IO — append-only**

`inputdev <L> <R> "<device>"` (written only when the address differs from the
default) and `trackchans <0|1>` (written only for mono). The `input` line keeps
its exact three fields, so a pre-M5 build still reads the project, and a pre-M5
file loads as the default input, channels 1-2, stereo.

**4. Recorder — one take per armed track, from the chosen node**

- `src/engine/AudioInputs.h` lists live input devices with
  `BMediaRoster::GetLiveNodes` (raw audio as the node's OUTPUT; the system mixer
  and physical output excluded) and resolves a stored name; it is shared with
  the input picker.
- `Recorder::Start(std::vector<TakeSpec>, deviceName)`: connects the CHOSEN node
  (a name that is not live falls back to the system default and says so), keeps
  pushing the whole interleaved capture into the ring, and the disk thread
  de-interleaves each take's channels into its own `WavWriter`. A frame split
  across two ring reads is carried rather than written shifted. The hook is
  unchanged in kind: no allocation, no locks, no I/O, no syscalls.
- Takes are 24-bit PCM, 32-bit float or 16-bit per `AppSettings::takeFormat`
  (`takefmt`, default 24-bit); exclusive create and the atomic error code are
  unchanged, so the disk-full path still stops the take and keeps what was
  written.
- The meter and the monitor ring follow the FIRST take's pair; the monitor ring
  stays **stereo-interleaved** (the engine's monitor path reads pairs) and is
  folded from the capture with the same kit-free `Deinterleave`.
- Per-take live waveform envelopes (only the first 4 takes; each costs ~2.4 MB),
  so each lane draws its own take as it grows.

**5. UI — one list, two consumers**

`TrackInputs.h` grows the audio choices: `None`, `Default Input`, then per live
device `<dev> 1-2` plus `<dev> Ch N` for each channel the device reports; the
current address is kept in the list when its device is not live. A single
channel means a mono track (`WidthOfChoice`), so both pickers commit the width
with the address. `SameInput` replaces the kind+name comparison in both
"mark the current choice" sites. The inspector and the arrangement header's In
field both still come from this one file.

## Verification (pasted)

```
$ cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release && cmake --build build-host -j2
build exit: 0
[100%] Built target projectio_fuzz_tests

$ ctest --test-dir build-host
100% tests passed out of 69
Total Test time (real) =  2.17 sec

$ sh scripts/haiku_syntax_check.sh
OK=74 FAIL=4
FAIL src/engine/FlacSink.cpp
FAIL src/engine/FlacSource.cpp
FAIL src/engine/VorbisSink.cpp
FAIL src/engine/VorbisSource.cpp
```

**The 4 FAILs are pre-existing and not this branch's:** those four files are
untouched here (`git diff --name-only` does not list them), and they include
`<FLAC/stream_encoder.h>` unconditionally while the cross toolchain's sysroot
has no FLAC/Vorbis headers (`scripts/haiku_syntax_check.sh` adds pkg-config
flags for lilv only). A pristine `git archive master` extraction run of the same
script gives 71 OK / 7 FAIL: the same four, plus `src/main.cpp`,
`src/ui/MainWindow.cpp` and `tests/ui_functional_tests.cpp`, which fail *only*
in that bare tree for the missing generated `Version.h`. In a configured tree
the baseline is therefore 4 FAIL, and this branch has the same 4 -- no new ones,
and 74 OK.

## Mutation checks

Each mutation was applied, the affected test run (it failed), then the mutation
reverted and the suite re-run green.

| Mutation | Caught by |
|---|---|
| `Deinterleave`: mono read `srcL + 1` | `channelsplit_tests` (mono extract) |
| `ClampToDevice` made a no-op | `channelsplit_tests` (device shrank) |
| `inputdev` line never written | `projectio_tests` (address round trip + text) |
| `trackchans` ignored on load | `projectio_tests` (mono round trip) |
| `NextFreeWavPaths` skipped the exists-check | `takenames_tests` (batch names) |
| `takefmt` dropped from `Serialize` | `appsettings_tests` |
| `SetTrackChannelsCommand::Undo` made a no-op | `track_depth_tests` |

Not mutation-checked, and why: everything in the Haiku-only half (node
enumeration, the connect, the hook, the disk loop's per-take writes, the
picker menus). Those cannot run on this host or on the dev VM -- neither has an
audio device -- so they are on the click list below, not in a claim of
verification. The kit-free pieces they are built out of (addressing, clamping,
de-interleave, naming, format mapping) are the mutation-checked ones above.

## Click list for Marc (hardware; no audio device exists on the host or the VM)

1. **Input picker.** On the HDA box, an audio track's `In` menu should list the
   live input devices with their channel counts, then the same list in the
   inspector. Check the channel count offered matches the device (the menu can
   only offer what the node reports; a node that reports nothing offers the
   default pair only).
2. **Multichannel capture.** Arm two (or more) mono tracks, point them at
   different channels of the interface (e.g. Focusrite in 2), record: each
   track must get its own `take-N.wav`, and each file must hold only that
   track's channel. A stereo track addressed to 1-2 must get a 2-channel file.
3. **Panned mono.** Record one channel to a mono track and pan it: the pan must
   behave as a mono source (constant-power), and the take must be 1-channel.
4. **Device unplugged.** Save a project whose track names a device, unplug it,
   record: the take must land from the system default with a note on stderr,
   not fail.
5. **Take depth.** Record at 24-bit (default) and check the take opens in
   MediaPlayer (the target image has no Media Kit readers -- check with our own
   playback, i.e. play the clip in the DAW). Float takes need the setting
   changed by hand until the Preferences window lands.
6. **Disk full.** Record to a small filesystem and fill it: the take must stop
   with the alert, and the file must keep what was written.

## Decisions, limits, and the hook another package needs

- **One capture has ONE device.** The first armed audio track that names one is
  what the whole pass records from; a second armed track naming another
  interface records the first one's channels rather than nothing. Recording
  several devices at once is not in M5's shape and is not built.
- **An armed audio track with no input picked records from the system default**,
  as it always did (a fresh track's `InputSource` is `kNone`, and honouring that
  as "record nothing" would have been a silent regression of arm-and-record).
  The `In` field still reads "None" for it -- worth revisiting when the
  Preferences window lands.
- **The take format default moves from 16-bit to 24-bit PCM** (plan M5.2 says
  24-bit or float; 24-bit is the sane recording default). `takefmt 0` restores
  the pre-M5 files exactly.
- **Mono/stereo is chosen by the channel pick**, not by a separate control: one
  channel = mono, a pair = stereo. The In field shows the device and channels.
- **The Preferences window is not built here** (another agent owns it). The hook
  it needs: `EnumerateAudioInputs()` (`src/engine/AudioInputs.h`),
  `AppSettings::takeFormat` (`takefmt`), `RecordController::fTakeFormat`, and
  `Recorder::Start(takes, device)`. The output-device picker (M5.3) is the same
  window's business.
- **Live waveforms are per take, but only for the first 4 takes** (~2.4 MB of
  envelope each); past that the region still draws, without the growing
  waveform. Peaks for the extra takes are built by the background loader unless
  one is already running (then they arrive at the next project load).

## Files

```
docs/agent-prompts/44-multichannel-input.md    the spec
docs/agent-prompts/44-multichannel-input-PR.md this record
src/model/Project.h          InputSource.address, TrackChannels, Track.channels
src/model/ChannelSplit.h     new: kit-free addressing + de-interleave
src/model/TakeNames.h        NextFreeWavPaths (a capture's names, one pass)
src/model/Commands.h/.cpp    SetTrackChannelsCommand
src/model/ProjectIO.cpp      inputdev / trackchans lines, append-only
src/app/AppSettings.h/.cpp   takeFormat (`takefmt`)
src/engine/AudioInputs.h     new: live input devices (Media Kit)
src/engine/Recorder.h/.cpp   chosen node, per-take writers, per-take envelopes
src/ui/TrackInputs.h         devices + channels in the ONE choice list
src/ui/InspectorView.cpp     commits the width with the address (SameInput)
src/ui/TimelineView.h/.cpp   per-lane live waveform, SameInput in its picker
src/ui/RecordController.h/.cpp  specs per armed track, drops per track
src/ui/MainWindow.h/.cpp     takefmt setting, background peak pass
tests/: channelsplit_tests.cpp (new), projectio_tests, takenames_tests,
        track_depth_tests, appsettings_tests
CMakeLists.txt               channelsplit_tests target
```
