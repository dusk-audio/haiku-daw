# Task: M6 file formats — AIFF/AIFC, FLAC and Ogg Vorbis import + export

You are working in `/home/marc/haiku-daw`, a native Haiku OS DAW. Work on branch
`feature/formats` off `master`. Two sibling agents (T1 theme, T2 dock browsers)
are editing `src/ui/MainWindow.cpp` and `src/ui/ExportWindow.cpp` in their own
worktrees at the same time — keep the UI hunks in those two files small, and in
as few of them as possible.

**Never the Media Kit.** `BMediaFile` and every Media Kit decoder stay
forbidden project-wide: the target image has no reader/decoder plugins
(`docs/HANDOFF.md`), so `BMediaFile` returns "No handler" for a plain WAV. Every
format here is our own parser or a bundled library. MP3 stays out of 1.0.

## Codebase orientation (read before coding)

- Layering: `daw_model` (model + DSP + engine decode/encode helpers) is kit-free
  and host-tested (`cmake -S . -B build-host && cmake --build build-host -j8 &&
  ctest --test-dir build-host`). `src/engine/Engine.cpp` is Haiku-only (Media
  Kit); **`src/engine/Exporter.cpp` is kit-free and host-testable — it is the
  executable spec for the write side, and `src/engine/WavSource.cpp` is the
  reader everything else should look like.**
- `WavSource` (`src/engine/WavSource.h`) is a hand-written RIFF/WAVE parser
  (PCM 8/16/24/32 + IEEE float) that streams interleaved stereo float, with
  `Open/IsValid/FrameRate/SourceChannels/TotalFrames/Seek/ReadChunk` and a
  native-channel variant for the sampler. Its defensive habits are the house
  style and must be copied: clamp the declared data size against the real file
  length, reject absurd channel counts, refuse a float format whose bit width is
  not 32, and never size a buffer from an untrusted header field.
- Its users: `Engine.h` (`TrackStream::fSource`, Haiku-only), `PeakCache`
  (kit-free `Build(WavSource&)`, used on every import for the waveform lane),
  `Exporter.cpp` (`DecodeClip`), `SampleBank.cpp`, `RenderJobs.cpp` (region ops)
  and `MainWindow::ImportAudioAt`.
- The import path: `MainWindow::ImportAudioAt(path, track, start)`
  (`src/ui/MainWindow.cpp:2569`) builds the clip from `TotalFrames()`/rate and
  builds the peak cache; the File ▸ Import Audio panel posts `MSG_IMPORT_REF`
  with an unfiltered `BFilePanel`; the drop handler is
  `TimelineView::MessageReceived` (`src/ui/TimelineView.cpp:204`) which today
  accepts a dropped file **by extension** (`DroppedExtIs(p, "wav")`).
- The write path: `ExportWav` (`src/engine/Exporter.cpp:169`) renders the whole
  project in RAM, applies normalization/limiting, then writes through
  `WavWriter` to `outPath + ".part"` and renames on success; `ExportStems`
  (`:693`) wraps it per track. `ExportFormat{bitDepth, dither}`,
  `ExportOptions`, `ExportJob` (progress + cancel) are in
  `src/engine/Exporter.h`. `ExportChoices` (`src/ui/ExportWindow.h`) is the
  dialog's state, posted to `MainWindow` as `kMsgExportOptions` and remembered
  in `AppSettings` (`exportBitDepth`, `exportDither`, `exportSampleRate`, …).
- Optional-dependency pattern: `DAW_LV2` (`CMakeLists.txt:88`) — an `option()`
  that auto-disables when the library is missing, a PUBLIC
  `DAW_HAVE_LV2=1` define so callers compile their own branch only where the
  dependency exists, and a build with the option OFF that must still be green.

## Goal

Import AIFF/AIFC, FLAC and Ogg Vorbis alongside WAV behind one interface chosen
by **sniffing the file's first bytes**, and export FLAC (16/24-bit) and Ogg
Vorbis (quality) alongside WAV — with both halves optional at build time, and
with the existing WAV behaviour, tests and byte output unchanged.

## Work items

1. **`IAudioSource`** (`src/engine/IAudioSource.h`, kit-free, header-only): the
   reader interface — `IsValid/FrameRate/SourceChannels/TotalFrames/Seek/
   ReadChunk`, i.e. exactly today's `WavSource` surface minus `Open` (the
   factory opens). `WavSource` derives from it; nothing else about it changes.
   `PeakCache::Build` takes `IAudioSource&` so every format gets a waveform.
2. **`AiffSource`** (`src/engine/AiffSource.{h,cpp}`, our own code, kit-free):
   AIFF and AIFC — `COMM` + `SSND`, 8/16/24/32-bit PCM (8-bit is **signed**
   here, unlike WAV) big-endian, the AIFC `sowt` little-endian variant,
   `twos`/`NONE`, and 32/64-bit IEEE float (`fl32`/`FL32`, `fl64`/`FL64`).
   Always `FORM` + `AIFF`/`AIFC`; `numSampleFrames` is untrusted and clamped to
   the bytes the file really holds; seeking is `SSND`-offset + frame size.
3. **`FlacSource`** (`src/engine/FlacSource.{h,cpp}`, libFLAC stream decoder)
   and **`VorbisSource`** (`src/engine/VorbisSource.{h,cpp}`, libvorbisfile).
   Both stream through the same `ReadChunk` contract; seeking is
   `FLAC__stream_decoder_seek_absolute` / `ov_pcm_seek` so the engine's
   per-clip source pre-seek keeps working for compressed media.
4. **`AudioFormats` + the import factory** (`src/engine/AudioFormats.{h,cpp}`):
   `enum class AudioFileFormat { Unknown, Wav, Aiff, Flac, Ogg }`, its name,
   label and file extension, per-format read/write availability (compile-time),
   `SniffAudioFileFormat(path)` reading the file's first bytes and
   `OpenAudioSource(path, &error)`. `FORM/AIFF`, `fLaC`, `OggS`, `RIFF/WAVE` —
   never the extension. An unavailable format is reported as exactly that, not
   as a corrupt file.
5. **Wire the import**: `MainWindow::ImportAudioAt`, the `TimelineView` drop
   handler (sniff, not `DroppedExtIs`) and the File ▸ Import Audio panel all go
   through `OpenAudioSource`; a file that cannot be opened raises the existing
   `ReportError` alert instead of only printing to stderr.
6. **`IAudioSink`** (`src/engine/IAudioSink.h`) + **`MakeAudioSink`**
   (`src/engine/AudioSinkFactory.{h,cpp}`): `Open(path, SinkFormat)` /
   `WriteFloat(interleaved, count)` / `Close()`. `SinkFormat` carries sample
   rate, channels, bit depth, float flag, dither and the Vorbis quality. The WAV
   sink wraps `WavWriter`; the FLAC sink is a libFLAC stream encoder; the Vorbis
   sink is `vorbis_encode_init_vbr` + `ogg_stream`. `Exporter.cpp` writes
   through the sink instead of `WavWriter`, so progress, cancel, normalization,
   limiting and **temp+rename are one code path for every format**.
7. **`ExportFormat`** gains the container (`Wav`/`Flac`/`Ogg`); `ExportWav` and
   `ExportStems` keep their names and signatures, `ExportStems` names a stem
   with the container's own extension. The dither PRNG moves to
   `src/engine/Dither.h` (`TpdfDither`, same xorshift32, same seed, so a
   dithered WAV stays byte-identical) and is used by both the WAV and FLAC
   16-bit paths.
8. **`ExportWindow`** offers the format: one **Format** menu (WAV 16-bit, WAV
   24-bit, WAV 32-bit float, FLAC 16-bit, FLAC 24-bit, Ogg Vorbis) built from
   the available formats only, one **Vorbis quality** menu (the bit-depth menu
   it replaces carried the same three WAV depths), plus the existing dither /
   loudness / true-peak / range / stems controls. `ExportChoices` gains
   `container` and `vorbisQuality`; `kMsgExportOptions` carries them; the
   remembered choices round-trip through `AppSettings` (append-only lines).
9. **Optional at build time**: `option(DAW_FLAC)` / `option(DAW_VORBIS)`,
   auto-disabled when the library is absent, PUBLIC `DAW_HAVE_FLAC` /
   `DAW_HAVE_VORBIS`. With a format off its sources are not compiled, the
   factory reports it unavailable and the dialog does not offer it. Unlike
   `DAW_LV2` the codecs are linked into `daw_model`, because the Exporter —
   which lives there and must stay host-testable — is the thing that needs the
   encoders. Recorded in the record as a deliberate deviation.

## Definition of done

- All existing ctests pass, and the WAV tests keep passing byte for byte.
- New host `formats_tests`: WAV/AIFF read back **sample-exact** against
  hand-written fixtures (16-bit BE, 8-bit signed, `sowt`, float), seeking on
  each, and a compressed round trip through the real sinks — FLAC 16- and
  24-bit sample-exact after quantisation, Ogg Vorbis lossy (frame count and an
  error bound, stated as such in the test). Truncated and corrupt files of every
  format fail cleanly; a format is chosen by content, not by extension; the
  exporter writes each container end to end with its temp+rename intact.
- Every new behaviour is **mutation-checked** (break it, watch the named test
  fail, restore) and the mutation is named in the commit body.
- `cmake --build build-host` **exit 0** and `ctest --test-dir build-host`
  green; the `b-asan` build green; `sh scripts/haiku_syntax_check.sh` 0 FAIL;
  VM `build` and `build-off` ctest green, plus a VM configuration with
  FLAC/Vorbis ON and one with that configuration OFF. Exact counts in the
  record.
- **UI:** `ExportWindow` changed, so per `docs/UI_GUIDELINES.md` the
  `DAW_UI_SHOTS` pass runs on the unlocked VM, a `Shot()` of the export dialog
  is added to `ui_functional_tests`, and every shot of it is opened and checked
  against §3 before this is called done. The shots reviewed are named in the
  record.
- The record states plainly which formats are verified end to end and which are
  compiled-and-not-available, and names anything Marc has to decide.
