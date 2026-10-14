# Haiku DAW — a short user guide

This is the tour, not the manual. Everything it describes is behaviour you can
find in the code; the in-app **View ▸ Keyboard Shortcuts** window is the
authoritative key list.

## The window

One window: a transport bar across the top, a **track inspector** down the left,
and the **timeline** filling the rest — with a docked pane along the bottom
(`J`) for the piano roll and the browsers.

- **Transport bar** — two rows. Row one: return-to-zero, rewind and
  fast-forward (one bar each), Play, Stop, Record, Loop, Click (the metronome)
  and Punch, then the zoom pair. Every button shows its own state, and the
  whole row behaves exactly like the keys below the buttons' names. Then the
  large **position display**: click it to cycle bars:beats → min:sec → samples
  → timecode; double-click it to type a position (Enter seeks; anything it
  cannot read is reported in the status line and nothing moves). Beside it:
  tempo and meter, the master volume, the master meter and the loudness
  readout. Row two is the **status line** — what just happened on the left
  ("Playing", "Recording take-3.wav", an error) and the readouts on the right:
  `DSP`, the peak time one audio callback took against the buffer's duration,
  and `XR`, the underrun count since the engine started.
- **Timeline** — one lane per track. The left gutter holds the track name,
  `M` mute, `S` solo, `R` record-arm, `I` input monitor, pan, the gain fader,
  and the track's meter. Right of it: the ruler, the clips and the playhead.
- **Inspector** — the selected track's input, inserts, sends, output, group,
  automation selectors, pan, fader, record/monitor and mute/solo, in signal
  order.

Menus: **File** (New, Open, Open Recent, Save, Save As, Import Audio/MIDI,
Export WAV/Stems/MIDI, Project Settings…, Preferences…, Close, Quit), **Edit**
(Undo, Redo, Cut/Copy/Paste/Delete, Select All, Duplicate, Insert Silence),
**Track** (New Audio Track, New MIDI Track, New Bus, Duplicate Track),
**View** (Mixer, Markers, Master Effects, Monitor Dim/Mono, Inspector,
Editor & Browsers, Zoom to Fit, Follow Playhead, Marker Lane, Keyboard
Shortcuts, Sample Browser), **Audio** (Metronome), **Help** (About).

The keys worth knowing: **Space** plays and stops, **Home** or **Enter**
returns to zero, **R** records, **L** loops the ruler's loop range, **C**
toggles the click, **S** splits at the playhead, **M** drops a marker, **F**
zooms to fit, **G** opens the grid menu, `1`–`6` pick the tools, and `,` `.`
nudge the selection by the grid step.

## Getting audio in

**Import a file:** File ▸ Import Audio… — it lands on the first audio track at
the playhead (or drag it in). WAV (PCM 8/16/24/32-bit and 32-bit float), AIFF,
and — in a build made with the libraries — FLAC and Ogg Vorbis. The format
comes from the file's header, not its extension, so a renamed file still opens.

**Record:** click `R` on a track to arm it (and `I` to hear it while
monitoring, if your input would otherwise be silent), then the Record button in
the transport. `I` works on its own too: with no transport running it monitors
that track's input live — arm nothing, record nothing, just listen (one input
device at a time, the first track whose `I` is on; a MIDI track's `I` plays its
instrument's live input the same way). **Preferences ▸ Monitor the input while
recording** is the session-wide version for takes. Count-in bars are in **Preferences** now (the Audio menu keeps
the metronome toggle); **drag on the ruler** to set the loop, and **Ctrl-drag**
there to set a punch range. A plain click on the ruler just moves the playhead —
**Clear Loop** and **Clear Punch** are entries of the ruler's right-click menu
(or the Loop button and `L` switch looping off). **L** with no loop range loops
the bar the playhead is in. Recording inside an enabled loop stacks takes you
can comp from — right-click a take for **Next Take**.

**Choose the input:** a track's `In` field lists what the system reports —
`None`, `Default Input`, then each live device with its channels. One channel
makes the track mono, a pair makes it stereo, and the choice is stored by
device *name*, so it survives a reboot or a renumbered node. Two armed tracks
pointing at two channels of the same interface record two takes in one pass,
each holding only its channel; a device that has gone away falls back to the
system default and says so on stderr rather than failing the take. Takes are
written as 24-bit by default (16-bit or float in Preferences), next to the
project, and the project refers to them by relative path — move the project and
its takes together and they still load.

**MIDI:** arm a MIDI track and pick its input in the inspector (the endpoint is
stored by *name*, so it survives a reboot). The track's instrument — a synth or
an SFZ/SF2 soundfont — is set in the instrument editor. Sustain (CC64),
pitch-bend and the mod wheel are audible, live and on playback.

## Editing

- **Clips:** drag to move, drag an edge to trim, drag the top corners to set
  fades. **Alt**-drag (or **Cmd**-drag) *through a clip's middle* slips it —
  the audio moves under the clip's window and the clip stays put (the edges and
  corners keep their own gestures). **Cmd**-drag (or **Alt**-drag) the right
  edge to *stretch* the clip — time-stretch, pitch kept, with the ratio drawn
  on the clip. Double-click a clip to rename it in place on its name strip
  (Enter commits, Esc cancels). Right-click for Copy / Delete / Split here /
  Rename / Mute / Fade Shape / Normalize / Reverse / Strip Silence / Clear
  Fades. Overlapping clips crossfade automatically, and **Shift** bypasses the
  grid for free placement. In a crossfade the click always takes the clip drawn
  on top.
- **Multi-select:** rubber-band or Cmd-click, then move/delete/duplicate as a
  group. Drag on empty lane area for a time range independent of the clips,
  then use Edit ▸ Cut/Copy/Paste/Delete or Insert Silence. A range and a clip
  selection are never both live: whichever you made last is what Delete acts
  on.
- **Notes:** double-click a MIDI region to open the **piano roll**. Its tool
  palette (keys `1`–`7`) picks pointer, pencil, brush, eraser, scissors, glue
  and velocity; the bottom lane shows velocity, **pitch bend**, or *any*
  controller — pick the number, and the menu spells out what each one does
  (the lanes a voice acts on today: mod wheel, volume, expression, pan, sustain;
  the rest say "no effect yet"). Drag a point to move it; right-click one to
  delete it, or use **Clear lane**. The **MIDI** button has the transforms —
  quantize (with grid, strength, swing and note ends), humanize, legato,
  transpose, velocity — and `q` re-runs the last quantize. Each transform is
  one undo step.
- **Piano-roll editing:** the grid field (key `g`) switches what edits snap to
  — bar, 1/2 … 1/32, triplets, or Off; **Shift** is still free placement.
  **Cmd-C/X/V/D** copy, cut, paste and duplicate notes (**Alt**-drag duplicates
  under the pointer too), the arrows nudge a selection by a grid step, `+`/`-`
  zoom about the playhead and `f` fits the region. The region's start and end
  are drawn, and notes the region does not play are dimmed — widening the
  region (drag its edge in the arrangement) brings them back. Clicking a key on
  the keyboard column, drawing a note, or clicking a velocity lollipop
  **sounds** the note through the track's instrument
  (Preferences ▸ "Preview notes when editing MIDI" turns that off). The roll
  follows the same playhead chase the arrangement does: View ▸ Follow Playhead
  drives both.
- **Normalize, Reverse, Strip Silence and Freeze** run in the background: the
  progress bar appears with a **Cancel**, the window stays usable while they
  work, and a cancelled job changes nothing.
- **Undo** is per gesture, everywhere, including mixing moves.

## Mixing

- **Faders and pans** are on the track headers and, in more detail, in the
  **Mixer** window. Each strip has the inserts, the send levels, an input
  **trim** knob with its value in dB, the phase `Ø` button, the `G` solo-group
  box, and **dB entry** — double-click a fader or its value to type a level.
  A send level takes effect on the next Play (the engine builds its send tables
  with the graph).
- **The mixer window** is a console: the master strip is pinned to the right
  edge, and anything wider than the window scrolls with the scrollbar (or the
  wheel over empty panel space). The strips follow the arrangement — a track
  added, deleted or reordered shows up at once.
  - Each strip's `In` opens the same input picker the inspector offers,
    `→Mst`/`→Bus` the same output picker, and `Auto` cycles the track's
    automation lane mode (the one the timeline's Auto box shows).
  - Every fader is a dB fader: the whole range from silence to +12 dB is on the
    travel, 0 dB sits at its own tick, and a typed level is never silently
    clamped.
  - The meters are stereo (one bar per channel) with a peak-hold tick and a
    clip lamp per strip; click the lamp to clear it, as on the master meter.
  - The keyboard drives it: click a strip (or `←`/`→`) to make it current,
    `↑`/`↓` nudge its gain by 0.5 dB, `M`/`S`/`R` toggle mute, solo and
    record-arm. Shift is the fine rate while dragging a fader, pan, trim or
    send knob, and while turning the wheel over one.
- **Buses and sends:** create a bus (Track ▸ New Bus), then give tracks an
  output or a send in the inspector. Pre-fader sends tap before the fader and
  its effects. Solo groups: put the same number in several tracks' `G` boxes
  and soloing one solos the group. The sends editor shows each level in dB and
  names the track it belongs to.
- **Inserts:** the inspector's insert slots. Click an empty slot to add a
  built-in effect, a native add-on, or an LV2 plugin; click a filled row to
  edit it (an LV2 plugin with its own interface opens *that*, and it drives the
  sound while you play). Each insert has a bypass dot and a wet/dry control —
  the wet/dry slider sounds while you drag it, like every other control in the
  panel. Double-click any control there (knob, parameter row, wet/dry or an EQ
  band handle) to put it back to its default, and hold Shift while dragging for
  a fine rate. An LV2 parameter with named values shows the plugin's own name
  ("Lowpass"), and a port whose name declares a unit ("Frequency (Hz)") shows
  it. Hovering or dragging an EQ band handle prints that band's frequency, gain
  and Q in the graph; the wheel over a handle turns its Q.
  The compressor and gate — and an LV2 plugin with sidechain inputs — take a
  **Key** from any other track.
- **Automation:** cycle a track's `Auto` box to the parameter you want, then
  click the lane to add points and drag them. Volume, pan and any effect
  parameter can be automated.
- The master strip has a BS.1770 loudness meter (momentary, short-term, true
  peak) so you can see what an export will measure. The master level is an
  ordinary undoable edit (Cmd-Z takes a fader move back) and marks the project
  as changed, so File ▸ Close prompts for it.

## Preferences and Project Settings

**File ▸ Preferences…** is where the session-shaping choices live: buffer size
(taking effect at the next Play), default sample rate for *new* projects,
count-in bars, the metronome's sound and level, monitor input, **preview notes
while editing MIDI** (the piano roll's audition), dark mode, the UI scale
(applied at once; window sizes settle at the next launch), the folder a first
Save starts in, and the autosave interval (0 turns it off). Audio
output still goes to the system default device — the row for it is disabled
until output-device selection lands.

**File ▸ Project Settings…** edits the project itself: its sample rate (with
**Resample the media to the new rate**, which writes a converted copy of every
distinct source beside the original and repoints the clips), tempo, meter and
free-text notes. Apply turns whatever you changed into **one** Edit ▸ Undo step
called "Project Settings" — a rate change is a whole-timeline rescale, so it
had better be one.

**File ▸ New** offers the templates shipped with the app: **Empty**, **Band**
(eight audio tracks with the drums routed to a bus) and **Producer** (two MIDI
tracks with instruments, three audio tracks, and a reverb bus the vocal and
drums send to). A new session starts at the default sample rate from
Preferences; the template brings tracks, names, routing and tempo.

## Exporting

File ▸ Export WAV… opens the export dialog: the format (WAV at 16/24-bit PCM or
32-bit float, FLAC, or Ogg Vorbis — whichever formats this build can write),
sample rate, dither, loudness normalization (target LUFS, true-peak ceiling,
limiter), the range (whole project or the loop), and mixdown or stems. Export
at another rate and the resample is a windowed-sinc filter, so nothing folds
back into the audible band. The choices are remembered.

The render runs on its own thread with a progress bar and a **Cancel** — the
window stays usable while it works, and a cancelled export leaves nothing
behind. The file appears only when it is complete.

File ▸ Export MIDI… writes the MIDI tracks as a Standard MIDI File.

## Projects and safety

- **File ▸ Save** writes one `.dawproj` text file; media paths inside it are
  relative to it. **Save As** moves it, **Open Recent** remembers the last ten,
  and Close/Quit/Open all prompt Save / Discard / Cancel when there is unsaved
  work — the title's `*` says when there is.
- **Autosave** writes a recovery copy every 30 seconds by default (never
  during a take) and offers it back if a session ends badly. The recovery file
  is removed on a clean quit, and preferences live in
  `~/config/settings/HaikuDAW/settings`.

## When something sounds wrong

- **The DSP readout** in the transport bar is the first thing to look at: if
  it sits near 100 %, raise the buffer size in Preferences (applied at the next
  Play) and watch the xrun count stop climbing.
- **A plugin that changes its reported latency while running** (some
  oversampling EQs do) is compensated: the host notices the move, re-latches
  and re-solves. The audible cost is one brief re-alignment when it changes,
  not a lasting offset or a click.
- **Freeze** a track (right-click its name) to render it, effects and all, to
  audio when a session gets heavy; it happens in the background with a progress
  bar.
