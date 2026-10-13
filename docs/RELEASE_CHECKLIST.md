# Release checklist — run on the real hardware

Everything here needs **ears, a real audio device, or a real installation**,
which is why it is not a test. Everything that a test *can* reach already is: the
host suites, the Kit-cross-compile check, and — since 2026-10-09 —
`ui_functional_tests`, which drives the real windows on the VM over SSH (see
`docs/agent-prompts/09-ui-functional-tests.md`). If an item below turns out to
be automatable, it should move there instead of being ticked here.

Machine: **192.168.1.186** (`ssh -i ~/.ssh/haiku_vm user@192.168.1.186`), 12
cores, HDA audio. It is Marc's physical box: ask before using it.

Getting the build there (the box may not reach GitHub; the host serves the
repo):

```sh
# on the Linux host
git bundle create /tmp/daw.bundle master
scp -i ~/.ssh/haiku_vm /tmp/daw.bundle user@192.168.1.186:/tmp/
# on the box
cd ~/haiku-daw && git fetch /tmp/daw.bundle master && git reset --hard FETCH_HEAD
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
```

Record the result of each item as **pass**, **fail** (what you heard/saw), or
**skip** (why). A failure gets a fix on a branch plus a regression test where
one is possible.

## A. Transport, playback and the P0 audio-safety fixes

1. **Play, stop, seek while playing, loop.** Load a project with several
   audio tracks; hit Play, click the ruler to seek while rolling, and let a
   loop run for a minute with the loop braces on the ruler.
   → No click, no blast of noise, no silence that stays silent.
   *(These are the P0 #1 NaN/denormal guards, the seek reset, and the loop
   seam.)*

2. **Faders and mute during playback (P0 #9).** With everything rolling, drag
   a track fader quickly up and down, and toggle M on two tracks repeatedly.
   → Level follows smoothly; no glitch, no stuck level, no crash.

3. **Denormal tail.** Put a long reverb on a bus, feed it one hit, and let it
   decay to silence with the transport still rolling.
   → The tail decays to silence; the meter does not freeze and no xrun is
   reported on stderr.

4. **Underrun recovery.** Set Audio ▸ Buffer Size to its smallest, load a
   heavy session (many tracks), and play.
   → If it underruns, it recovers: the audio resyncs to the playhead instead
   of drifting or dying. Watch stderr for the xrun line.

5. **Metronome and count-in.** Turn the metronome on, set a count-in of two
   bars, and record.
   → Clicks are on the beat and the take starts at the right bar.

## B. Recording, monitoring and latency (R3)

6. **Round-trip compensation.** Arm an audio track with `I` (input monitor) on,
   and play a sharp sound (clap, click, snare) on the input while recording,
   monitoring through headphones.
   → The recorded transient lands **where you heard it**, not visibly late. A
   few milliseconds of doubt is fine; a smeared double-hit is not.
   *(This is `DeviceRoundTripFrames` — it reported 0 on the VM, which is why
   this item exists. If it sounds late, the number the device reports is wrong
   or unused.)*

7. **Punch record.** Set a punch range (Ctrl-drag on the ruler), arm, record
   across it.
   → Only the punch range is replaced, and the new audio is in time with what
   surrounds it.

8. **Loop record and take comping.** Enable a loop over a bar, record four
   passes.
   → Four takes stacked; right-click ▸ Next Take switches between them, each in
   time with the loop.

9. **Autosave during a take.** Start a long take (over 30 s), stop, then quit
   without saving and relaunch.
   → The recovery offer appears and restores the project; the take it
   references is complete, not half-written.

10. **Freeze.** Right-click a busy track ▸ Freeze; play.
    → It sounds the same as before the freeze; Unfreeze restores the original
    content exactly.

## C. MIDI

11. **External keyboard.** Play the keyboard into an armed MIDI track with the
    instrument editor open.
    → Notes sound while you play; nothing is dropped at fast passages.

12. **CC playback (the never-listened CC work).** Import a `.mid` that uses CC7
    (volume), CC10 (pan) and CC11 (expression); play it.
    → Volume and pan follow the file; a CC7 step glides rather than clicks.

13. **Panic at stop.** Hold a note on the keyboard (or with the mouse in the
    piano roll) and press Stop.
    → Sound stops; nothing hangs.

14. **Transforms.** In the piano roll: `q`, then MIDI ▸ Humanize / Legato /
    Transpose / Velocity, and Edit ▸ Undo after each.
    → Each one changes what you hear and undoes in one step.

## D. Mixing and routing

15. **Buses and sends.** Route two tracks through a bus with a reverb on it,
    and a pre-fader send from a third.
    → The bus fader controls both; the pre-fader send follows the track's
    input, not its fader.

16. **Automation.** Draw a volume ramp on a track and a parameter ramp on an
    insert; play.
    → Both follow the drawing, and the insert's own window (if it has one)
    shows the parameter moving.

17. **Monitor dim/mono.** Toggle View ▸ Monitor: Dim and Mono on the master.
    → Both change the output, and the loudness meter stays honest.

## E. Plugins (LV2)

18. **Insert and edit.** Add 4K EQ 2 to a track, open its own window, drag a
    control while playing.
    → The sound changes as you drag; on release (about half a second) it
    becomes one undo step; closing the window while playing is clean.

19. **Automation reaches the editor.** With that editor open, draw an
    automation lane for one of its parameters and play across it.
    → The plugin's own control follows the automation.

20. **Bypass and wet/dry.** Toggle an insert's bypass dot, and drag its
    wet/dry.
    → Both are audible, and undoing returns exactly to the previous sound.

21. **A latent plugin.** Put **Limiter** (the look-ahead limiter in the insert
    list) on a bus and play material through it.
    → No timing shift against the dry tracks (that is PDC), and the ceiling
    holds.

## F. Export

22. **A bounce sounds like playback.** Export the whole project to 24-bit WAV
    and play the file.
    → It is the mix you heard, at the same length and alignment.

23. **Loudness and limiter.** Export with normalization to −14 LUFS and the
    limiter on, then measure the file (or play it beside a reference).
    → It is louder than the raw mix, no peak exceeds the ceiling, and nothing
    pumps audibly.

24. **Stems.** Export stems of a four-track project; import them onto new
    tracks at the same start.
    → They sum back to the mixdown within a hair.

25. **Cancel.** Start a long export and press Cancel.
    → It stops within a moment, and there is no file (and no `.part` file) at
    the destination.

## G. Robustness and the app itself

26. **Crash recovery.** Kill the app with unsaved changes (or lose power to
    it), relaunch.
    → The recovery offer appears; accepting restores the session.

27. **A corrupt project file.** Truncate a `.dawproj` by hand and open it.
    → The app says so and keeps the current session; it does not crash or
    half-load.

28. **Long session.** Leave a project playing for half an hour with the
    meters and the mixer window open.
    → No drift, no leak that shows as slowing, no xrun storm.

29. **Version.** `daw --version` and Help ▸ About.
    → Both say 1.0.0.

## H. Packaging and installation

30. **Install from the package.** Build the `.hpkg` on the VM (`haikuporter`
    with the release tag, or the recipe's build by hand), copy it to the box,
    install it (`pkgman install haiku_daw-1.0.0-1-x86_64.hpkg`).
    → The app is in the Deskbar's Applications menu, `daw --version` and
    Help ▸ About both say 1.0.0, and **File ▸ New Project…** offers Empty, Band
    and Producer — the shipped templates, i.e. the package put them where the
    app looks (`$dataDir/haiku-daw/templates`, `src/app/ProjectTemplates.cpp`).
    A missing template directory is a non-event (the dialog falls back to the
    built-in Empty row), so seeing three rows is what says the install is
    complete.

31. **Launch from the Deskbar.** Click the application in the Deskbar's
    Applications menu.
    → The window opens at a usable size with no Terminal involved; the entry
    reads "Haiku DAW" and, once the HVIF icon lands, shows it rather than the
    generic application icon.

32. **Open a project by double-click in Tracker.** Save a project, quit, then
    double-click the `.dawproj` in Tracker; then repeat with the app already
    running.
    → First case: the app starts and opens the project (title without `*`).
    Second: the running instance takes it — `B_SINGLE_LAUNCH` — and no second
    copy appears; a project opened while the current session has unsaved
    changes raises the Save/Discard/Cancel prompt first.

## I. The 1.0 features that need ears or eyes

*(The plan's M8.3 list also names the unsaved-changes prompt; that flow is
already driven by `ui_functional_tests` (M0.1's test) and is deliberately not
repeated here. Same for anything else the suite reaches.)*

33. **Disk full during recording.** Point the project at a filesystem with no
    room left (a small image is easiest) and record a 24-bit stereo take.
    → The take stops with the "Recording" alert, the app stays up, and what
    was written is still there: the file plays and sits on the timeline up to
    the failure. (The Recorder's write error reaches `ReportError` and the
    partial take is landed through `FramesWritten()`.)

34. **Multichannel record.** Arm two mono tracks, point each `In` field at a
    different channel of the same interface, record; then a stereo track
    addressed to channels 1–2.
    → Each mono track gets its own take holding only its channel, the two are
    in time with each other, and the stereo take is one 2-channel file. With
    the Focusrite: it may not appear in the In menu at all — Haiku's
    `usb_audio` driver is USB audio class 1 only (`README.md`, known
    limitations), and the 2i2 3rd Gen is UAC2 — in which case record the skip
    and run the same check from the HDA box's own inputs, which are what this
    app is developed against.

35. **FLAC and Ogg export played back.** Export the loop range as FLAC and as
    Ogg Vorbis, then play both files.
    → Both play at the length and level you heard, with nothing ragged at the
    start or end. Where MediaPlayer has no decoder plugins (`docs/HANDOFF.md`:
    the dev VM image ships none) play them back in the DAW instead — its own
    readers are the ones that matter — and say which machine each check ran on.

36. **Sidechain ducking.** Put the compressor (or gate) on a bus, give it a key
    from a drum track (the insert's Key row), and play the drum against
    material on the bus.
    → The bus ducks on each hit, in time (the key is delay-aligned to the
    consumer's input), and clearing the key restores it. A track keyed from
    itself changes nothing.

37. **A stretched loop stays in time.** With a loop range set, Cmd-drag (or
    Alt-drag) a clip's right edge to stretch it to about twice its length, then
    play through the loop for several passes.
    → It stays with the metronome, the loop seam does not stumble, and every
    pass sounds the same; the clip shows its `×2.00` badge. (The per-pass
    alignment is what M7.2's seek determinism buys.)

38. **A sustain pedal.** Play a keyboard into an armed MIDI track and hold the
    sustain pedal (CC64) while pressing and releasing notes; then play the
    recorded region back.
    → Notes ring on after key-up while the pedal is down and stop when it is
    released, live and on playback, with the pedal recorded as a controller
    event. Same with the piano roll's own notes if no pedal is at hand: draw
    CC64 into the lane of a region and play it.

39. **A dynamic-latency plugin.** Put **4K EQ 2** on a track (it switches
    oversampling at runtime), play material against another track, and toggle
    oversampling on and off while it plays.
    → The transport keeps rolling, the two tracks never end up *permanently*
    offset, and the re-alignment is the documented short step when the latency
    moves (`docs/agent-prompts/46-dynamic-latency-PR.md`), not a lasting shift
    or a click.

40. **Every window at 100% and 150%.** Run the `DAW_UI_SHOTS` pass on the box
    (screen unlocked — a locked screen draws nothing), once per theme mode, and
    once with the Preferences UI scale at 150%; then open every shot of every
    window you changed.
    → Nothing overlaps, no label is clipped or runs past its control, and every
    new row is legible at both sizes. This is the pass M1.3/M1.4 merged
    without; it is not a formality.

## What a pass means

Ticking every item here, on this machine, with the suites green on master, is
the release. Anything that fails gets fixed with a test where the test can
reach it, and the item re-run — by hand if it must be.
