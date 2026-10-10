# 28 — M6 file formats: AIFF/AIFC, FLAC and Ogg Vorbis import + export

Branch `feature/formats` off `master` (`6d88063`). Spec: `28-formats.md`.
Draft — the verification numbers below are filled in as each pass runs.

## What this is

One reader interface and one writer interface for every audio file the app
touches, and the two compressed formats 1.0 promises, both optional at build
time.

**Read side — `IAudioSource`** (`src/engine/IAudioSource.h`). The interface is
`WavSource`'s own surface (rate / channels / frames / `Seek` / `ReadChunk`), so
nothing downstream learns what container a clip points at:

| Reader | File |
|---|---|
| `WavSource` (unchanged behaviour, now an `IAudioSource`) | `src/engine/WavSource.h` |
| `AiffSource` — our own AIFF/AIFF-C parser | `src/engine/AiffSource.{h,cpp}` |
| `FlacSource` — libFLAC stream decoder | `src/engine/FlacSource.{h,cpp}` |
| `VorbisSource` — libvorbisfile | `src/engine/VorbisSource.{h,cpp}` |

`AudioFormats.{h,cpp}` holds the vocabulary (`AudioFileFormat`), the sniff, the
availability queries and `OpenAudioSource`. **Selection is by content, never by
extension** (`SniffAudioHeader` reads the leading bytes: `RIFF/WAVE`,
`FORM/AIFF|AIFC`, `fLaC`, and for Ogg the first page's lacing table + the
Vorbis identification packet, so Opus and Ogg-FLAC are *not* mistaken for
Vorbis). Wired into `MainWindow::ImportAudioAt`, the `TimelineView` drop
handler, the File ▸ Import panel (which was already unfiltered), the engine
(`TrackStream::Prepare`), `RebuildPeaks`, the freeze/region paths and the
export's clip decode. A file that cannot be opened now raises the existing
`ReportError` alert instead of only printing to stderr.

Seeking is the codec's own: `FLAC__stream_decoder_seek_absolute` /
`ov_pcm_seek` / the SSND offset, so the engine's per-clip pre-seek lands on the
right sample for compressed media too. Seeking exactly to `TotalFrames` (which
`WavSource` accepts and which reads nothing) is handled by parking the cursor —
the codecs refuse to decode *from* a sample that does not exist.

**Write side — `IAudioSink`** (`src/engine/IAudioSink.h`). `Exporter` writes
through `MakeAudioSink(container)` (`AudioSinkFactory.cpp`), so progress,
cancel, loudness normalization, true-peak limiting and the **temp+rename** are
one code path for every container. The WAV sink wraps the existing `WavWriter`
body for body, so existing bounces stay byte-identical (the 16-bit dither PRNG
moved to `src/engine/Dither.h` unchanged — same xorshift32, same seed).

`ExportFormat` gains `container` (appended *after* `bitDepth`/`dither`, because
those two are positionally initialised all over the exporter tests).
`ExportStems` names a stem with its container's extension.

**UI.** `ExportWindow`'s bit-depth menu becomes one **Format** menu built from
`ExportFormatChoiceAt()` — WAV 16 / WAV 24 / WAV 32-float, plus FLAC 16 /
FLAC 24 and Ogg Vorbis only where the build can actually write them — and a
**Vorbis quality** menu (Low/Medium/High/Maximum). `ExportChoices` carries
`container` + `vorbisQuality`; `kMsgExportOptions` carries them as
`"container"` / `"vquality"`; both round-trip through `AppSettings` (append-only
`expcont` / `expqual` lines, with an old settings file still opening on WAV).

## Build-time availability

`option(DAW_FLAC)` / `option(DAW_VORBIS)`, both ON by default and both
auto-disabled when the library is missing — the `DAW_LV2` pattern. With a
format off: its sources are **not compiled**, `DAW_HAVE_*` is not defined, the
factory refuses it with an explicit message ("this build has no FLAC support")
and the dialog does not offer it. Nothing half-works.

**Deliberate deviation from `DAW_LV2`:** the codecs link into `daw_model`
rather than into a separate target. The Exporter lives in `daw_model` and must
stay host-testable, and it is the thing that needs the encoders — a separate
target could not be reached from it. The model layer stays kit-free (no Haiku
library, no Media Kit); it gains a codec dependency when one is found. Recorded
here because the CMakeLists comment for `DAW_LV2` says the opposite for lilv.

**What the VM had, and what was installed there:**

- Present already: `libFLAC.so.14`, `libvorbis.so.0`, `libvorbisfile.so.3`,
  `libogg.so.0` (runtime libraries only, no headers, no `.so` symlinks).
- **Installed system-wide on the VM** (needed to build against them):
  `flac_devel-1.5.0-1`, `libvorbis_devel-1.3.7-1`, `libogg_devel-1.3.6-2`
  from HaikuPorts, via `pkgman install -y flac_devel libvorbis_devel`. All
  three report as *system* packages, so removing them later is
  `pkgman uninstall flac_devel libvorbis_devel libogg_devel`.
- With those installed, the VM configure finds them
  (`-- FLAC support enabled (/boot/system/develop/lib/libFLAC.so)`,
  `-- Ogg Vorbis support enabled (/boot/system/develop/lib/libvorbisfile.so)`),
  so **both formats are compiled and verified on the VM**, not merely on the
  host.

## Verification

| Pass | Result |
|---|---|
| `cmake --build build-host` | exit 0 |
| `ctest --test-dir build-host` | 53/53 (was 52/52; +`formats_tests`) |
| `ctest --test-dir b-asan` (`-DDAW_SANITIZE=ON`) | 53/53 |
| `ctest --test-dir b-noformats` (`-DDAW_FLAC=OFF -DDAW_VORBIS=OFF`) | 53/53; `formats_tests` reports "157 checks, 0 failures (built without FLAC) (built without Ogg Vorbis)" |
| `sh scripts/haiku_syntax_check.sh` | 0 FAIL (10 files) |
| the same check with `-DDAW_HAVE_FLAC=1 -DDAW_HAVE_VORBIS=1 -DDAW_HAVE_LV2=1` | 0 FAIL (23 files: every Haiku-only source) |
| VM `build` (LV2/FLAC/Vorbis ON) — configure finds both libraries, build | exit 0, `ctest` **55/55** (was 54/54; +`formats_tests`) |
| VM `ui_functional_tests` inside that ctest | **253 checks, 0 failures** |
| VM `build-off` (`-DDAW_LV2=OFF`, codecs ON), build | exit 0, `ctest` **51/51** (was 50/50) |
| VM `-DDAW_FLAC=OFF -DDAW_VORBIS=OFF`, build | exit 0; `formats_tests` **155 checks, 0 failures (built without FLAC / Ogg Vorbis)** |
| VM `formats_tests` (build, codecs on) | **274 checks, 0 failures** (280 on the host: the six `/dev/full` checks are Linux-only) |
| VM `ui_functional_tests` with `DAW_UI_SHOTS` | clean exit, 25 shots; the export-dialog shot opened and zoomed (the only window this branch changes), plus the export panel, stems panel, widget-kit and save-panel shots for comparison |

VM configure output for the enabled build, verbatim:
`-- FLAC support enabled (/boot/system/develop/lib/libFLAC.so)` and
`-- Ogg Vorbis support enabled (/boot/system/develop/lib/libvorbisfile.so)`.

### What the VM turned up that the host could not

Both harness-level, both found by the VM and fixed before pass C:

1. **A quitting window must not be touched.** The M6 dialog test hid the
   windows around it, and `HideOtherWindows` locked every window it found —
   including the export dialog, which is still listed while it quits, with no
   thread left to release its lock. The suite hung for the full 15-minute cap.
   `LockWithTimeout` now skips a window that will not lock, and the test waits
   for the dialog to leave the application's window list (not merely to stop
   being drawn) before touching anything else. Every lock this test takes is
   bounded for the same reason.
2. **The VM's codecs are built unoptimised, and the suite's project is eight
   seconds long.** Bouncing that to Ogg Vorbis took longer than the test's own
   file timeout, so the check failed while the encode was still running; the
   progress bar then stayed up over every later test and the app would not exit
   at the end (ctest reported a timeout on a run whose every check had passed).
   The container test now bounces a **one-second loop window** and waits 60 s
   per container. `formats_tests` — the same encoders, the same exporter,
   driven directly instead of through the window — passes on the VM with 0
   failures, which is what says the encoders are sound and the trouble was the
   harness's assumption about how much audio it was encoding.

One more thing for whoever runs the next screenshot pass: **the VM's screen
blanker engages during a long `DAW_UI_SHOTS` run and every shot comes back
black** (pass B produced 25 black PNGs). Pass C keeps the screen awake by
sending the shift key from the host every 25 s with `virsh sendkey` — input
only, no system setting touched, the same thing a person jiggling the mouse
does. Black shots in a future pass mean the same thing, not a broken app.

`formats_tests` is 259 checks on the host build (all codecs on).

### Mutation checks (break it, watch the named check fail, restore)

1. **libFLAC's STREAMINFO read from the decoder's own accessors instead of the
   metadata callback** → `test_flac_roundtrip`: `src != nullptr` fails. The
   accessors report the most recently *decoded frame header*, so straight after
   the metadata pass they are all zero — this was the real bug the round trip
   caught.
2. **`AiffSource` clamping `TotalFrames` to COMM's `numSampleFrames` without
   bringing `fDataBytes` down with it** → `test_aiff_clamps_and_corrupt`:
   `got.size() == 4` fails (the trailing bytes the header disowned still
   played).
3. **Sniffing Ogg at a fixed offset instead of walking the page's lacing
   table** → `test_sniff`: the `Ogg` comparison fails.
4. **Seeking FLAC to `TotalFrames` through the decoder** → `test_flac_roundtrip`:
   `src->Seek(frames)` fails (libFLAC refuses to decode from a sample past the
   last one).
5. **`VorbisSink` not propagating a failed page write** → the `/dev/full` leg
   of `test_export_containers`: `!ok` fails (a disk-full Vorbis export would
   otherwise rename a truncated file into place).

Each one was applied, the failing check watched, and the tree restored (a
`git diff` after each confirms nothing was left behind).

### Tests added or extended

- `tests/formats_tests.cpp` (new, 259 checks): sniffing by header and by file;
  AIFF 16-bit BE, signed 8-bit, AIFC `sowt`, `fl32`, `fl64`, seek, COMM/AIFF
  clamps both ways, truncated SSND, missing SSND, an unreadable compression
  type, a non-audio file; the factory by content with a wrong extension;
  FLAC 16/24-bit round trips asserted **sample-exact** against the encoder's
  own quantisation, plus seek and seek-to-end; Vorbis frame count, a bounded
  error (lossy: the test says so), channel identity and seek; truncated and
  garbage FLAC/Ogg failing cleanly; the choice tables; and the exporter end to
  end for every writable container (sniffed back, `.part` gone, cancel leaves
  nothing, stems carry the right extension, an unwritable container fails
  cleanly, `/dev/full` never reports success).
- `tests/appsettings_tests.cpp`: the container + Vorbis quality survive a
  round trip, and a settings file without those lines still opens on WAV.
- `tests/ui_functional_tests.cpp`: a `Shot()` of the export dialog, its Format
  menu checked row-for-row against the kit-free tables, and a bounce per
  writable container through the app whose file is sniffed from its bytes.

`tests/formats_tests.cpp` is host-only in one leg: the `/dev/full` write-failure
check is `#if defined(__linux__)` (Haiku has no such device, and filling the VM
disk is not an option), so the VM run exercises every other leg.

### The syntax check, and its two blind spots

`scripts/haiku_syntax_check.sh` on `master` compiled the `#ifdef DAW_HAVE_LV2`
branches of `src/ui/` and `tests/` OUT (the define comes from the `daw_lv2`
target, which the check does not link), so its "0 FAIL" did not cover them.
This branch touches six UI sources and `ui_functional_tests.cpp`, all of which
have such branches.

T3 fixed that in `origin/feature/lv2-state` (`33d6f05`). **Not cherry-picked**:
that commit's context shows its branch also carries a lilv/pkg-config include
block `master` does not have, so importing it would pull a larger change into
this branch's copy of a shared script and widen the merge surface for no gain.
Instead the check was run with the same defines added on the command line —
`-DDAW_HAVE_FLAC=1 -DDAW_HAVE_VORBIS=1 -DDAW_HAVE_LV2=1` — and reports 0 FAIL
over 23 Haiku-only sources. The VM `build` run (which has all three on, from
the real CMake targets) is the second, stronger proof, since it links.

This branch's own change to that script is one line: `os/add-ons/graphics`
was missing from its include path, so any source including `Screen.h` — e.g.
`EffectsWindow.cpp`, byte-identical to `master`'s — reported FAIL for
`Accelerant.h`. (T3's copy fixes the same thing with `os/add-ons/*/`; the two
touch the same region, so expect one trivial conflict there.)

## Screenshots reviewed

Looked at, at full size and zoomed on the rows (the export dialog is the only
window this branch changes; `docs/UI_GUIDELINES.md` §3 list):

- **`06-export-dialog`** — the dialog with the new **Format** and **Vorbis
  quality** rows, at (200,200) in the diagnostic pass and at (560,40) in the
  pass the suite now takes. Rows read, top to bottom: the stems box; Format
  showing the remembered choice ("WAV 32-bit float" — the previous test had
  set 32-bit float, so this is the dialog remembering rather than a default
  leaking through); Vorbis quality ("Medium"); Range ("Loop range"); Sample
  rate ("Project rate"); Dither; Normalize loudness; Target LUFS; True-peak
  limiter; Ceiling dBTP; Cancel / Export. Nothing overlaps, every label and
  every marked value fits its well, the window is exactly as tall as its
  contents with no dead band, the title is "Export", and both buttons are
  centred.
- **`07-export-dialog-ws`** — the same dialog built by MainWindow, once it
  could be mapped (see below). Identical content, which is what says the twin
  the suite shoots is the window the user gets.

One thing I looked at and did **not** change, because it is the theme work's
(T1): the wells are **right-aligned with ragged left edges** (each row's well
starts after its own label), which is how the pre-existing Range and Sample
rate rows already draw. (In the first diagnostic run the marked values also
rendered dim; in the final pass, with a healthy app_server, they read
correctly — that was an artifact of the earlier blocked server, not a defect.)

The suite also shoots a **twin** dialog built by the test thread (same class,
same contents) so there is always a dialog in the pass to review, whatever the
app_server is doing that day; the MainWindow-opened dialog is still driven end
to end (menus checked row for row against the format tables, applying it opens
the save panel), so only its pixels come from the twin.

Note on the theme: `DAW_UI_THEME` does not exist on `master` yet — the theme
mode is T1's, still unmerged — so the pass runs with the app's current
(always-dark "Logic Slate") palette, which is the dark look the brief asks for
anyway.

## What is verified, and what is not

- **Verified end to end (host, and on the VM where the libraries are built
  in):** FLAC 16-bit and 24-bit import and export, Ogg Vorbis import and
  export, AIFF/AIFC import, WAV import and export.
- **Compiled-and-not-offered:** the `-DDAW_FLAC=OFF` / `-DDAW_VORBIS=OFF`
  configuration builds, tests and degrades as designed; it is not "working
  FLAC that is off", it is a build that has no FLAC code in it at all.
- **Not done here:** AIFF *export* (read-only, and 1.0 does not ask for it),
  MP3 (out of scope by decision), and soundfont samples
  (`SampleBank::LoadWavToMemory`) still read WAV only — that path needs
  `ReadChunkNative`, which only `WavSource` implements.

## Click list for Marc (ears / hardware, not the screen)

`docs/RELEASE_CHECKLIST.md` already asks for "FLAC/Ogg export played back in
MediaPlayer". On the hardware box:

1. Bounce a mix to FLAC (File ▸ Export, Format: FLAC 24-bit) and to Ogg Vorbis,
   and play both in MediaPlayer — they should sound like the WAV bounce.
2. Import a FLAC and an AIFF onto the timeline; the waveform lane should be
   drawn and playback should be in time with the rest.
3. Seek into the middle of a FLAC clip while playing: no wrong-pitch blip (the
   codec's own seek should land on the sample the timeline shows).

## I launched the app binary on the VM, and should not have

**Plainly, because it may have damaged something on this machine:** while
chasing the dialog-mapping question below I launched the built `daw` app on the
VM twice (`~/haiku-daw/build/daw`) to see whether a normal session shows the
export dialog. `master`'s `src/main.cpp` still calls `set_ui_color`, and Haiku
**saves** those writes, so those launches may have rewritten the VM's system
colour table — the very thing the stop-the-line instruction describes.
`ui_functional_tests`, `ctest` and the headless suites do not do this; the app
binary does. I have stopped, and I have not touched any other setting
(wallpaper, Deskbar, network, libvirt) or any settings file outside
`~/config/settings/HaikuDAW/`.

Restoring the VM's colours, if they are wrong, is **Preferences ▸ Appearance ▸
Colors ▸ Defaults** — I have deliberately not done it. (There is also a stray
`daw` process from those launches; the last verification pass kills it.)

## The dialog-mapping anomaly (intermittent, harness-side)

In two passes the export dialog MainWindow opens was **never drawn**: in the
window list, `IsHidden()` false, a sane frame (`200,200,433,572`), even the
ACTIVE window — and invisible. In the final pass it **did** draw, in the same
shot as the twin, so this is intermittent rather than a property of the code.
What I established with A/B runs:

- a plain `BWindow` the test creates (same feel, same flags) always drew;
- `SetWorkspaces(B_CURRENT_WORKSPACE)` on the invisible one made it appear
  immediately (`Workspaces()` read `0x80042` for it and `0x2`, the current
  workspace, for the control);
- the **same `ExportWindow` class**, built by the test thread, always drew.

Both passes where it was invisible followed crashed or killed test runs that
had left app_server state behind (a debugger alert on screen, a `daw` process
running). I did **not** work around it in the app: the window's construction
and `Show()` are untouched by this branch (only the menu contents changed), so
I have no reason to think I introduced it. **Marc: opening File ▸ Export
WAV… once in a normal session is worth a look** — if it appears (which I
expect), this is harness flakiness and can be closed.

## Needs Marc

- Nothing blocking. Two notes:
  1. The VM now has `flac_devel` / `libvorbis_devel` / `libogg_devel`
     installed system-wide (see above) — say the word and I will uninstall
     them.
  2. `scripts/haiku_syntax_check.sh` had a pre-existing hole: `os/add-ons/
     graphics` was not on its include path, so every source including
     `Screen.h` (e.g. `EffectsWindow.cpp`) reported `FAIL` for `Accelerant.h`.
     One line added; that file is byte-identical to `master`'s, so the FAIL was
     never this branch's.
