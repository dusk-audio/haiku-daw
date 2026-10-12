# Task: Multichannel audio input (M5)

You are working in a Haiku DAW checkout, C++17/CMake. Branch `feature/multichannel-input`
off `master`. `docs/PLAN_1.0.md` §M5 is the source of truth; this file is how it is built.

## Codebase orientation (read before coding)

- Layering: `daw_model` (model + DSP + exporter) is kit-free and builds/tests on this
  Linux host (`cmake -S . -B build-host && cmake --build build-host -j2`,
  `ctest --test-dir build-host`). `src/engine/Recorder.cpp`, `src/ui/` and the Media Kit
  half of the engine are Haiku-only: no host compile — cross-check with
  `sh scripts/haiku_syntax_check.sh` (must report 0 FAIL) and stay pattern-faithful.
- Recording today: `RecordController` (`src/ui/RecordController.cpp`) arms tracks
  (`fRecTracks`), opens one `Recorder` (`src/engine/Recorder.*`) per take, and on stop
  drops the one take file on EVERY armed track. The Recorder connects
  `BMediaRoster::GetAudioInput` with a wildcard format, converts each buffer to float and
  pushes it into a lock-free `RingBuffer`; a disk thread drains it into one `WavWriter`.
  **RT contract (`Recorder.h`): the hook allocates nothing, locks nothing, does no I/O.**
- Take naming: `NextFreeWavPath` (`src/model/TakeNames.h`) — never overwrite (M0.3).
- The Recorder's disk failure path is a contract: the disk thread sets an atomic
  `ErrorCode()`, the UI pulse reports once and calls `StopRecording()`, which keeps
  `FramesWritten()` samples of the file. Do not regress it.
- Input choices are ONE list, `src/ui/TrackInputs.h`, consumed by the inspector's input
  button (`InspectorView.cpp`) and the arrangement header's "In" field
  (`TimelineView.cpp`, `OpenInputPicker`). A second copy of that list is the bug the
  header exists to prevent.
- IO is append-only line-based text (`src/model/ProjectIO.cpp`): a new field is a new
  optional line, written only when non-default, ignored by older builds. Compat tests use
  the `LoadPatched` pattern (`tests/projectio_tests.cpp`, `tests/instrument_io_tests.cpp`).
- Model edits go through `CommandStack` (`src/model/Commands.h`).

## Goal

Record from a *chosen* input device, capture *all* of its channels, and write ONE take
per armed track — a mono take from one device channel, or a stereo take from a channel
pair — so an 8-in interface can feed eight mono tracks in one pass.

## Work items

1. **Model** (`src/model/Project.h`):
   - `InputSource` gains `std::string device` (the Media Kit input node's NAME, matched
     by name like a MIDI endpoint so it survives id reassignment; `""` = system default)
     and `int channelL` / `int channelR` (0-based channels ON that device). `channel`
     stays the MIDI channel filter; the audio meaning moves to channelL/channelR.
   - `Track` gains `TrackChannels channels` (`Mono`/`Stereo`, default `Stereo` = today's
     behaviour). A mono track records from `channelL` only. Playback needs no engine
     change: `WavSource` fans a 1-channel file out to L/R, and `EqualPowerGains` is the
     mono (-3 dB centre, constant power) law, so a mono take pans as a mono source.
2. **Kit-free channel math** (`src/model/ChannelSplit.h`, header-only, host-tested):
   `TakeFormat` (16-bit / 24-bit / float32), `TakeSpec` (path, stereo|mono, srcL, srcR,
   format), `PlanTake` (a track's address -> spec), `ClampToDevice` (an address against
   the channels the capture actually negotiated), `Deinterleave` (interleaved device
   block -> one take's mono/stereo block; out-of-range channel reads silence).
3. **IO** (`src/model/ProjectIO.cpp`): append-only optional lines
   `inputdev <channelL> <channelR> "<device>"` (audio tracks, written when non-default)
   and `trackchans <0|1>` (written only for mono). An older file loads with the defaults
   (default input, channels 1-2, stereo); an older build ignores the new lines.
   Round-trip + legacy-compat tests in `tests/projectio_tests.cpp`.
4. **Recorder** (`src/engine/Recorder.*`, Haiku-only):
   - enumerate input nodes with `BMediaRoster::GetLiveNodes` (raw audio as the OUTPUT
     filter — an input device produces the capture stream) instead of `GetAudioInput`;
     `src/engine/AudioInputs.h` holds the enumeration, shared with the input picker;
   - `Start(std::vector<TakeSpec>, deviceName)`: connect the CHOSEN node (by name; a name
     that is gone falls back to the system default), capture every channel into the ring,
     and de-interleave each take's channels into its own `WavWriter` on the disk thread;
   - takes are written as 24-bit PCM or 32-bit float (`AppSettings::takeFormat`); the
     monitor ring stays stereo-interleaved (the engine's monitor path reads pairs), fed
     from the FIRST take's channels;
   - per-take live waveform envelopes (the UI draws a growing take), disk-full keeps its
     current behaviour: atomic error, UI stops the take, what was written is kept.
5. **UI** (`src/ui/TrackInputs.h` + its two consumers):
   - audio choices become: None, Default Input, and per enumerated device
     `<device> 1-2` plus `<device> Ch N` for each channel the device reports (mono);
     the device's current address is kept in the list even when the device is absent;
   - one `SameInput` comparison for the two "mark the current choice" sites, so a
     channel entry does not light up for its sibling;
   - `InputShortLabel` shows the device tail plus the channel pair on a mono/odd address.
6. **Wire-up** (`src/ui/RecordController.cpp`, Haiku-only): build one `TakeSpec` per armed
   audio track (its own device/channels, its mono/stereo width, the take format), pass the
   device name of the first armed track to `Start`, and on stop drop each take on its OWN
   track (loop-record and punch paths included). `RecordController::fTakeFormat` is the
   setting; `MainWindow::LoadSettings/SaveSettings` persist it as `takefmt`.

## NOT in this package

- **The Preferences window** (its audio-device picker) belongs to the parallel agent
  building it. The hook it needs: `EnumerateAudioInputs()` in `src/engine/AudioInputs.h`,
  `AppSettings::takeFormat`, and `Recorder::Start(takes, device)`. Whichever lands second
  wires the window.
- **The output-device picker** (M5.3) — same window, same owner.

## Definition of done

- Host: `cmake --build build-host -j2` exits 0, `ctest --test-dir build-host` green.
- `sh scripts/haiku_syntax_check.sh` reports 0 FAIL.
- Every new behaviour has a test, mutation-checked (break it, watch the test fail, restore).
- **Device enumeration and the capture itself cannot run on the host or on the dev VM
  (no audio device there).** They go on the click list for the HDA box
  (192.168.1.186) and the Focusrite, as M5.4 says — not into a claim of verification.
- Record: `docs/agent-prompts/44-multichannel-input-PR.md`.
