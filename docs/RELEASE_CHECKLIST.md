# Release checklist — run on the real hardware

Everything here needs **ears or a real audio device**, which is why it is not a
test. Everything that a test *can* reach already is: the host suites, the
Kit-cross-compile check, and — since 2026-10-09 — `ui_functional_tests`, which
drives the real windows on the VM over SSH (see
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

## What a pass means

Ticking every item here, on this machine, with the suites green on master, is
the release. Anything that fails gets fixed with a test where the test can
reach it, and the item re-run — by hand if it must be.
