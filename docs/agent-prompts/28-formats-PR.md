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
| `sh scripts/haiku_syntax_check.sh` | 0 FAIL |
| VM `build` (LV2/FLAC/Vorbis ON) ctest | _pending_ |
| VM `build-off` (`-DDAW_LV2=OFF`) ctest | _pending_ |
| VM `build-noformats` (`-DDAW_FLAC=OFF -DDAW_VORBIS=OFF`) ctest | _pending_ |
| VM `ui_functional_tests` (checks) | _pending_ |
| `DAW_UI_SHOTS` pass | _pending_ |

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

## Screenshots reviewed

_pending_ — the `DAW_UI_SHOTS` pass runs on the unlocked VM and every shot
showing the export dialog is opened against `docs/UI_GUIDELINES.md` §3.

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
