# Handoff — Haiku DAW

Read this first, then `docs/ARCHITECTURE.md`. This is a working project with
a running audio engine; you are continuing it, not starting it.

**Touching the UI?** Read `docs/UI_GUIDELINES.md` first. Reviewing screenshots
of every window you change is part of done — tests passing is not enough.

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
- **Phase A — mixer routing** ✅ (host-tested where kit-free; engine/UI build on
  VM): master-bus FX (A1); bus tracks + per-track `output`, topo-sorted graph in
  both the RT engine and offline Exporter (A2); aux `sends[]` with a general
  edge topo `ResolveOrderWithEdges` — Exporter honors pre/post-fader exactly,
  RT engine does post-fader, SendsWindow + "Snd" header box (A3); monitor
  Dim/Mono, RT-safe, applied post-metering (A4); BS.1770-4 master loudness meter
  — momentary/short LUFS + true-peak dBTP live readout, integrated disabled on
  the audio thread for RT-safety (A5). See `docs/ROADMAP.md` Phase A.
- **Phase B — automation** ✅ (gain + pan; host-tested where kit-free): per-track
  `gainAuto`/`panAuto` lanes + `SetAutoLaneCommand` + IO (B1); Exporter
  per-sample envelope, RT engine per-block lane-driven fader (B2); header "Auto"
  box + breakpoint editing on the lane (B3). Effect-param/send-level automation
  deferred. See `docs/ROADMAP.md` Phase B.
- **Phase C — editing depth** ✅ (host-tested where kit-free): per-clip gain
  (C1), clip split (C2), track reorder (C3), per-track color + variable lane
  height (C4), automatic overlap crossfade (C5, kit-free ComputeCrossfades),
  clip multi-select + group move/delete/duplicate via MacroCommand (C6). See
  `docs/ROADMAP.md` Phase C. New kit-free helpers: `Crossfade.h`, `MacroCommand`.
- **Phase T — tempo & meter map** ✅ (step changes; host-tested where kit-free):
  kit-free `TempoMap` (frame-anchored, exact frame<->beat, meter-aware BBT) +
  Project/IO (T1); grid/ruler/snap walk the map (T2); metronome follows tempo +
  meter (T3); right-click-ruler markers to add/remove tempo/meter changes (T4).
  Tempo ramps + BBT transport readout deferred. New kit-free header: `TempoMap.h`.
- **Phase D — recording depth** ✅ (audio; host-tested math + compile-checked;
  runtime pending on VM): overdub (record runs the engine), count-in (D1),
  punch-in/out (D2), input monitoring (D3, rate-matched), loop-record + take
  comping (D4). Kit-free `RecordPlan.h`; new `IMonitorSource.h`. See ROADMAP.
- **TESTING** — `scripts/haiku_syntax_check.sh` compile-checks the Haiku-only
  sources with a local `x86_64-unknown-haiku-g++` (under `~/haiku-cross/`); run
  it before every commit that touches engine/UI. On the VM, `sh scripts/vm.sh
  test` builds and runs the whole suite there, and `ui_functional_tests` drives
  the real windows over SSH (a GUI process on that VM does reach app_server).
  See `docs/agent-prompts/09-ui-functional-tests.md`.
- **Phase E — MIDI depth** ✅ (instrument; host-tested + VM-verified): per-track
  `Instrument` (waveform + ADSR), rewritten stateless Synth, InstrumentWindow
  editor. External MIDI-in + sample/wavetable synth deferred. New: `Instrument.h`.
- **Phase G — UI polish (first pass)** ✅ (VM-verified): vertical track scroll,
  zoom-to-fit (F), BBT transport readout, keyboard-shortcuts help.
- **Phase F — sample browser** ✅ (VM-verified): `src/storage/BfsAttr` (BFS
  attributes + fs indexes), `SampleBrowser` live BQuery window, import auto-tags
  duration. New dirs: `src/storage/`.
- **Phase H — persistence & robustness** ✅ (VM-verified): `AppSettings`
  (prefs + window layout), take bundling next to the project, autosave + crash
  recovery. New dir: `src/app/`.
- **UI overhaul + review** ✅ (VM-verified): per-track metering, dark palette +
  color-striped headers, fully custom-drawn mixer (faders/meters/master/dB),
  piano-roll MIDI editor (`src/ui/PianoRoll.*`, double-click a MIDI track name).
  Full-codebase code review fixed 8 real bugs (EQ NaN, delay Inf, self-send
  routing collapse, MacroCommand data-loss undo, AppSettings zero-clobber,
  WavSource OOB, WavWriter race, monitor UAF) — all with regression tests.
- **Undo unification + effect-param automation** ✅ (VM-verified): command
  coalescing makes fx/sends/instrument/notes/color/height undoable;
  `IEffect::SetParam` + `Track.fxAuto` + engine/Exporter per-block drive + timeline
  Auto-box editing give full effect-parameter automation. `Eq::MagnitudeResponseDb`
  drives the EQ graph; custom effects editor (knobs/EQ graph/comp curve).
- **Production-readiness pass** ✅ (committed on `master`; audio-listening verify
  pending) — a deep audit reframed the gap to shippable as stability/durability,
  not features. P0 crash/data-loss/audio-safety blockers (all 10) + the P1
  correctness batch are fixed with regression tests + fuzz corpora; export
  mastering (Phase X: bit-depth + dither + loudness normalization) is under way.
  Full detail + what's next in **`docs/PRODUCTION_HANDOFF.md`**; the phase/parity
  roadmap in **`docs/ROADMAP.md`**.
- **NEXT** — finish Phase X (export selection UI + worker thread), Phase Y latency
  compensation, Phase Z MIDI depth (external Midi Kit 2 I/O), Phase I plugin
  hosting; plus the deferred list (MIDI-note multi-select, tempo ramps, resampled
  input monitor, drag-drop from the sample browser). Runtime-verify the GUI + the
  committed engine changes in the VM (`sh scripts/vm.sh ssh`, launch
  `~/haiku-daw/build/daw`).

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

The target image (Haiku R1/beta6, x86_64) ships Media Kit
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

Claude edits + builds/tests kit-free code on Marc's **Linux host**. Haiku runs
in a **libvirt QEMU/KVM** VM (`haiku-beta6`, 2 vCPU / 2 GB, 192.168.122.48).

Code moves with **`sh scripts/vm.sh`**, which puts the host's current branch on
the VM's checkout (or `VM_REF=<ref>`): a commit already pushed is fetched by the
VM **from GitHub** (`origin`, public, through the VM's NAT), and one that is not
pushed goes over in a `git bundle`. `sh scripts/vm.sh ssh '<cmd>'` runs a
command there, `build`/`test` sync and build. The host is authoritative; the VM
never commits.

**Running an uncommitted tree on the VM** (to look at a change before
committing it): `vm.sh sync` carries commits, not a dirty tree. Make a
throwaway commit object without touching the branch, and sync that:
```
git add -A && CT=$(git commit-tree $(git write-tree) -p HEAD -m wip) \
  && git reset -q --mixed HEAD && git update-ref refs/heads/_vmwt $CT
VM_REF=_vmwt sh scripts/vm.sh build
git update-ref -d refs/heads/_vmwt        # afterwards
```
(`git stash create` misses untracked files; keep scratch build dirs out of
the repo root or `git add -A` sweeps them in.)

The repository **does have** a GitHub remote: `dusk-audio/haiku-daw` (created
2026-10-09). Push branches and master; never force-push.

## Hard-won lessons (don't repeat these)

- **Do NOT touch the VM's libvirt network config.** Attempts at passt and
  bridged networking and qemu `custom-argv` each broke Haiku's boot. Plain
  user-NAT is the working setup, and SSH into the VM works over it (it is how
  `scripts/vm.sh` and the functional tests drive the machine).
- The VM decode failure was the missing media plugins (above), not the
  code — verified by MediaPlayer also failing and by `media_probe.sh`.
- **A locked VM screen draws nothing.** The app_server never asks a window to
  draw while the screen is locked: draw timings print nothing and screenshots
  show the lock screen. Visual review and draw measurements need it unlocked
  (or the hardware box, 192.168.1.186).
- **...and the blanker comes back DURING a run.** `screen_blanker` covers the
  display after ~10 idle minutes, and a full `DAW_UI_SHOTS` pass is now longer
  than that: the shots taken late in the pass come out solid black while the
  early ones are fine (found 2026-10-13 — the three new windows' shots were the
  black ones, i.e. exactly the ones nobody had seen before). Kill it
  repeatedly for the run, not once before it:
  `(while true; do for p in $(ps | grep "[s]creen_blanker" | awk '{print $2}'); do kill $p 2>/dev/null; done; sleep 20; done) &`
  Note the second `ps` field: Haiku prints the path first, so `awk '{print $1}'`
  feeds `kill` a path and silently does nothing.
- **A killed dirty run wedges the next one.** Killing `ui_functional_tests` or
  the app with unsaved changes leaves
  `~/config/settings/HaikuDAW/recovery.dawproj`; the next start opens a modal
  Recover? alert on the window thread and every locked check times out. Delete
  it before a run — and note that the USER'S OWN app recreates it every 30 s
  (its autosave), so a suite run on a machine where someone is working can lose
  the race no matter what is deleted first: since `feature/ux-pass` the suite
  removes the file BEFORE the application object exists (the window reads it
  while it is being built) and answers a Recover? prompt that still appears
  with Discard, so the run keeps its own project and the screenshots keep the
  suite's fixtures. **And keep an eye on the PATH**: both removals were built by
  appending a leaf to `ProjectDocument::SettingsPath`, which names the settings
  FILE — `BPath::Append` appends, it does not replace a leaf, so they removed
  `…/HaikuDAW/settings/recovery.dawproj` and had been no-ops since the day they
  were written. A run therefore inherited the last one's scale, panes and window
  frame, which is how a "clean slate" run failed three scale checks and a
  piano-roll click that the run before it passed (2026-10-13).
- **…and so does the SETTINGS file (`~/config/settings/HaikuDAW/settings`).**
  It persists the panes (`uiinsp`, `uidockpage`), so a run that ended with the
  inspector hidden makes every later run start with it hidden: the pane tests
  that assume it is visible fail, and the run after them (a plugin pick whose
  message targets the inspector's view) fails too. Delete the file before a
  run, or let the suite do it (it does since `feature/arrange-depth`).
- **A pipeline hides a failure: check the exit code yourself.** `sh scripts/vm.sh
  build` and `test` used to end in `... | tail -3`, so a failed compile (or a
  failing `ctest`) exited 0 and left the PREVIOUS binaries in place — a stale
  binary run that looks green. The script now writes the build log, tails it and
  `exit`s with the real status (fixed on `feature/arrange-feel`, 2026-10-10);
  any other command you build from a pipeline in a script needs the same
  treatment. `set -e` does not save you: the pipeline's status is its LAST
  command's.
- **A FAULTED run wedges the next one just as hard.** A thread that trips a
  Haiku assertion (`debugger()`) raises the "has encountered an error" dialog,
  and until it is answered every later run on that machine hangs — ctest
  reports the run as `Signal 21`, which is `SIGKILLTHR`, i.e. the Terminate the
  dialog sends, not the fault. Look at the screen
  (`virsh -c qemu:///system screenshot haiku-beta6 /tmp/x.png`) and clear it
  with `virsh -c qemu:///system qemu-monitor-command haiku-beta6 --hmp 'sendkey ret'`
  (Terminate is the default button; `sendkey` is a keyboard event, not a
  settings change — changing the debug server's settings is NOT allowed).
  The fault itself is in the kernel's log: `sh scripts/vm.sh ssh 'grep -A 20
  DEBUGGER /boot/system/var/log/syslog | tail -30'` prints the assertion and
  the stack, which is far faster than bisecting.
- **`debugger()` from the app means one of two shapes.** `BView`/`BLooper` call
  `check_lock()`/`_CheckOwnerLockAndSwitchCurrent()` and assert when a view
  method runs off the window lock ("Looper must be locked.") or on a view with
  no owner ("View method requires owner and doesn't have one." — e.g. drawing
  into a `BBitmap` built without `B_BITMAP_ACCEPTS_VIEWS`). Tests must hold the
  window lock around every view call; a view that draws into a bitmap needs
  `acceptsViews` and `Lock()` before `AddChild`.
- **Green tests did not mean a working UI.** M1.3/M1.4 merged with the main
  window visibly broken and every check passing. `docs/UI_GUIDELINES.md` is
  the rule that came out of it.
- **A synthetic click at a hard-coded offset can land on a control that opens a
  synchronous popup** — and `BPopUpMenu::Go` does NOT return until a choice is
  made, so the window thread blocks and every later check that wants the window
  lock times out (it reads as 8 unrelated failures and a wedged run). The
  arrange-depth package's mid-drag check pressed the header fader at
  `laneTop + Themed(56)`; the arrange-tracks package moved the fader and left
  an input picker on that pixel. Tests must ask the view for a control's own
  rect (`HeaderRectFor`, `HeaderControlAt`) instead of computing one.
- **A posted mouse message needs `screen_where`, or the view sees (0, 0).**
  `BWindow::DispatchMessage` converts a mouse message's SCREEN point into the
  view-local `be:view_where` that the kit's `BView::MessageReceived` actually
  reads, and a message carrying only `"where"` is not converted at all — the
  view's `MouseDown` gets (0, 0), which lands on whatever sits in the corner
  (usually the toolbar) and the gesture "does nothing" while every later check
  in the test fails for reasons of its own. Post both points, converting with
  `BView::ConvertToScreen`; `TestPianoRollCcLane`'s `clickLane` is the working
  precedent, and package 51's `TestRollModelSync` lost three rounds to it.
- **A posted key needs the FOCUS token, not the window's.**
  `BWindow::_IsFocusMessage` requires `B_PREFERRED_TOKEN`, so
  `BMessenger(window).SendMessage(key)` never runs the menu's shortcut path and
  the key dies in `BWindow::MessageReceived` — which does not handle keys, so
  the shortcut "does nothing" and the check can never pass. Post through the
  preferred handler (`BMessenger((const BHandler*)nullptr, window)`), which is
  what a real app-server event carries; `TestRollModelSync`'s `postKey` is the
  precedent. (For a view's own KeyDown, posting to the VIEW works too — that is
  `BMessenger(view)` and `BView::MessageReceived` translates it.)
- **A pop-up menu CAN be driven by a test — this is the recipe.** The suite
  long said "a popup a test cannot open" and worked around every picker by
  calling the method behind the menu item, which left the pick itself (and the
  crash that lived in it) untested. It works like this: a posted mouse-down on
  the FIELD opens the pop-up (`BPopUpMenu::Go` runs the tracking on a thread of
  its own, so the window's thread sits in `Go()` until the menu closes); the
  pop-up's window is a `BMenuWindow`, which `be_app->CountWindows()` filters
  OUT, so it is found through its looper instead — `get_next_thread_info(0, …)`
  plus `BLooper::LooperForThread`, matched on the thread name a `BWindow`
  derives from its title (`"w>" + title`; every `DawMenuField` pop-up is
  `"pick"`); and the row is taken with the kit's own keyboard navigation (Down
  moves the selection, Return takes it: `BMenu::KeyDown` → `fChosenItem` → the
  item `Go()` returns), posted with the FOCUS token as above. Two traps, both
  from the same fact — the tracking loop reads the POINTER and the BUTTONS from
  the input server, never from messages: (a) a pick made within
  `get_click_speed()` of the open is read as the click that opened the menu,
  which then re-tracks and throws the pick away — wait the click speed out; and
  (b) the selection starts on the row under the real pointer, so ask the menu
  where its pointer is (`BMenu::GetMouse` + `BMenuItem::Frame`) and count the
  Downs from there instead of assuming none. `TestPreferences`'s `pickRow` is
  the precedent. Older comments in `tests/ui_functional_tests.cpp` that say a
  popup cannot be driven predate this and are about the pickers that never got
  converted.
- **A monitor-only graph is the ONLY thing rendering while it is up.** The
  engine has two RT bodies: `RenderSegment` (playing, recording, the in-place
  join) and `RenderMonitorOnly` (the idle monitor — live MIDI voices for an
  armed track's I lamp, and the roll's audition). Anything shared between them
  has to live in both: the input monitor's mix was written into the playback
  path alone, so an audio track's I lamp raised a monitor-only graph that
  rendered the live voices and NOTHING of the input it was raised for — the
  button was silent, and no state check could see it (the flag, the source and
  the state line were all correct). `Graph::MixMonitorInput` is now shared;
  `TestInputMonitor` drives it with a fake `IMonitorSource` and reads the master
  peak, which is the only way to check the mix on a machine with no input
  device.
- **A gesture's modifiers belong on the EVENT — never `BView::modifiers()`.**
  The kit's global `modifiers()` asks the input server what is held *right
  now*: it is neither the modifiers the gesture was made with (a Shift released
  mid-drag changes it) nor anything a posted message can set, so a synthetic
  Alt- or Ctrl-drag reports none and the handler takes its plain-click path —
  the gesture is unreachable, and only real fingers could ever drive it. Read
  the event with the app's own `EventModifiers(Window()->CurrentMessage())`
  (`src/ui/UiMetrics.h`), as the arrangement's gestures already did;
  `PianoRollView::EventMods` is the precedent.
- **Two agents share this VM, and so do its scratch paths.** `refs/heads/_vmwt`
  (the HANDOFF's own throwaway-ref recipe), `~/haiku-daw`, its `build/` dir and
  `/tmp/ui.log` are common state: a sync left in an earlier `flock` hold is
  overwritten by the sibling's next sync, and a run that does not rebuild in
  the SAME hold executes the sibling's binary (two runs reported 329 checks and
  "0 failures" for a tree this package never built). Sync, build, run and copy
  the log to a private name INSIDE ONE hold, and use a per-agent ref name.

## Commit rules

- Author **`marc@duskaudio.com`** (audio project) or `marc@lunacoap.com`.
- **Never** add a `Co-Authored-By:` trailer. No AI attribution in history.
- Conventional-commit style subjects (`feat(engine): ...`, `fix(...)`).

## M4 state (how the UI is built — read before touching it)

> **This section is history, not the current UI.** It describes the window as
> it was at M4 (one strip, no menu bar, rebuild-on-play). Since then: the menu
> bar, the split `MainWindow` (M1.1), the theme and widget kit (M1.2/M1.3), the
> Layout Kit dialogs with the inspector and dock panes (M1.4), the two-row
> transport bar, the arrange-window work, and the engine graph that swaps in
> place (M4.1). The current tour is `docs/USER_GUIDE.md`, the design of record
> `docs/ARCHITECTURE.md`, and the per-item state the status table in
> `docs/agent-prompts/README.md`.

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

## Next task — the 1.0 push

The milestone ladder (M0–M8 + resampler + DSP) and feature phases A–H (mixer
routing, automation, editing depth, recording depth, tempo/meter map, MIDI
instrument, sample browser, persistence) are **done**, and so is the 1.0 plan
that followed them: `docs/PLAN_1.0.md` M0–M7 with the UI foundation, the
arrange window, the transport/mixer/preferences layer, the engine work
(off the window thread, a seamless loop, the stream pool, background peaks, the
sinc resampler, dynamic latency), multichannel input, the compressed formats,
sidechain, time-stretch and MIDI expression. What remains is the release
itself: `docs/RELEASE_CHECKLIST.md` on the hardware, the tag, and the three
items the plan's status header names (M1.5 unmeasured, M1.6's HVIF bytes,
M5.3 not built). Per-item state: `docs/agent-prompts/README.md`.

Keep every milestone runnable; keep the audio thread real-time-safe; commit as
`marc@duskaudio.com` with no AI trailer; ship via the git-pull loop.
