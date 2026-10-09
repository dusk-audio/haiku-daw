# Haiku DAW — a short user guide

This is the tour, not the manual. Everything it describes is behaviour you can
find in the code; the in-app **View ▸ Keyboard Shortcuts** window is the
authoritative key list.

## The window

One window: a transport bar across the top, a **track inspector** down the left,
and the **timeline** filling the rest.

- **Transport** — Play/Stop, the time readout (bars:beats and mm:ss), the
  master fader, the loudness/true-peak readout, and the master meter.
- **Timeline** — one lane per track. The left gutter holds the track name,
  `M` mute, `S` solo, `R` record-arm, `I` input monitor, pan, the gain fader,
  and the track's meter. Right of it: the ruler, the clips and the playhead.
- **Inspector** — the selected track's input, inserts, sends, output, group,
  automation selectors, pan, fader, record/monitor and mute/solo, in signal
  order.

Menus: **File** (Open, Save, Import Audio/MIDI, Export WAV/Stems/MIDI),
**Edit** (Undo, Redo, Paste), **Track** (New Audio Track, New MIDI Track, New
Bus), **View** (Mixer, Metronome, Master Effects, Monitor Dim/Mono, Zoom to
Fit, Follow Playhead, Keyboard Shortcuts, Sample Browser), **Audio** (buffer
size, count-in, monitor input), **Help** (About).

## Getting audio in

**Import a file:** File ▸ Import Audio… — it lands on the first audio track at
the playhead (or drag it in). WAV only: PCM 8/16/24/32-bit and 32-bit float.

**Record:** click `R` on a track to arm it (and `I` to hear it while
monitoring, if your input would otherwise be silent), then the Record button in
the transport. Count-in bars are in the Audio menu; **drag on the ruler** to
set the loop, and **Ctrl-drag** there to set a punch range (a bare click in the
ruler clears the punch). Recording inside an enabled loop stacks takes you can
comp from — right-click a take for **Next Take**.

Recorded takes are written next to the project as WAV files, and the project
refers to them by relative path — move the project and its takes together and
they still load.

**MIDI:** arm a MIDI track and pick its input in the inspector (the endpoint is
stored by *name*, so it survives a reboot). The track's instrument — a synth or
an SFZ/SF2 soundfont — is set in the instrument editor.

## Editing

- **Clips:** drag to move, drag an edge to trim, drag the top corners to set
  fades. Right-click for Copy / Delete / Split here / Normalize / Reverse /
  Strip Silence / Clear Fades. Shift constrains, and overlapping clips
  crossfade automatically, and **Shift** bypasses the grid for free placement.
- **Multi-select:** rubber-band or Cmd-click, then move/delete/duplicate as a
  group.
- **Notes:** double-click a MIDI region to open the **piano roll**. Its tool
  palette (keys `1`–`7`) picks pointer, pencil, brush, eraser, scissors, glue
  and velocity; the bottom lane shows velocity or a controller (Vel/Vol/Expr/
  Pan). The **MIDI** button has the transforms — quantize (with grid, strength,
  swing and note ends), humanize, legato, transpose, velocity — and `q`
  re-runs the last quantize. Each transform is one undo step.
- **Undo** is per gesture, everywhere, including mixing moves.

## Mixing

- **Faders and pans** are on the track headers and, in more detail, in the
  **Mixer** window.
- **Buses and sends:** create a bus (Track ▸ New Bus), then give tracks an
  output or a send in the inspector. Pre-fader sends tap before the fader and
  its effects.
- **Inserts:** the inspector's insert slots. Click an empty slot to add a
  built-in effect, a native add-on, or an LV2 plugin; click a filled row to
  edit it (an LV2 plugin with its own interface opens *that*, and it drives the
  sound while you play). Each insert has a bypass dot and a wet/dry control.
- **Automation:** cycle a track's `Auto` box to the parameter you want, then
  click the lane to add points and drag them. Volume, pan and any effect
  parameter can be automated.
- The master strip has a BS.1770 loudness meter (momentary, short-term, true
  peak) so you can see what an export will measure.

## Exporting

File ▸ Export WAV… opens the export dialog: sample rate, bit depth (16/24-bit
PCM or 32-bit float), dither, loudness normalization (target LUFS, true-peak
ceiling, limiter), the range (whole project or the loop), and mixdown or stems.
The choices are remembered.

The render runs on its own thread with a progress bar and a **Cancel** — the
window stays usable while it works, and a cancelled export leaves nothing
behind. The file appears only when it is complete.

File ▸ Export MIDI… writes the MIDI tracks as a Standard MIDI File.

## Projects and safety

- **File ▸ Save** writes one `.dawproj` text file; media paths inside it are
  relative to it.
- **Autosave** writes a recovery copy every 30 seconds (never during a take)
  and offers it back if a session ends badly. The recovery file is removed on a
  clean quit.
- **Preferences** (window layout, buffer size, audio settings, the export
  dialog's last choices) live in `~/config/settings/HaikuDAW/settings`.

## When something sounds wrong

- **Buffer size** (Audio ▸ Buffer Size) is the first knob: smaller is lower
  latency, larger is more forgiving of a busy machine.
- **A plugin that changes its reported latency while running** (some
  oversampling EQs do) cannot be compensated exactly; the audible effect is a
  tiny timing offset while it works, not a glitch.
- **Freeze** a track (right-click its name) to render it, effects and all, to
  audio when a session gets heavy.
