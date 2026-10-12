// Exporter — offline "bounce" of a Project to a stereo file in one of the
// formats this build can write (WAV PCM 16/24-bit or 32-bit IEEE float, FLAC
// 16/24-bit, Ogg Vorbis at a chosen quality).
//
// Renders the whole timeline to disk with no real-time constraints: no
// BSoundPlayer, no ring buffers, no disk threads. It reuses the same kit-free
// building blocks the live Engine uses (IAudioSource, Resampler, Synth,
// EffectFactory) and replicates the Engine's mix math — equal-power pan,
// per-track fx chain, master gain — so an export sounds like playback.
//
// Everything is done in-RAM: offline rendering values simplicity and exactness
// over memory footprint. Kit-free (STL + the project's own classes + whichever
// codec libraries the build enabled) so it builds and unit-tests on any host.
//
// The file is finished by an IAudioSink (src/engine/IAudioSink.h): the WAV,
// FLAC and Vorbis writers all sit behind that one interface, so the temp file,
// the rename on success and the cancel path are the same for every container.
// The entry points below keep their historical names — `ExportWav` is the mix
// bounce whatever container it is told to write.
#pragma once

#include "../model/Project.h"
#include "AudioFormats.h"

#include <atomic>
#include <functional>
#include <string>

namespace daw {

// The output sample format of a bounce.
//
// The first two members are in their historical order on purpose: this is an
// aggregate, and `ExportFormat{32}` / `ExportFormat{16, true}` appear all over
// the exporter and render tests. A new member goes at the END.
struct ExportFormat {
    // 16 or 24 = PCM, 32 = IEEE float (WAV only; FLAC clamps 32 to 24);
    // anything else falls back to 16. Ignored by Ogg Vorbis, which is lossy.
    int  bitDepth = 16;
    // TPDF dither at the 16-bit LSB. Ignored at 24/32 (ample headroom: the
    // quantizer's own error is ~-144 dBFS there) and by Ogg Vorbis.
    bool dither   = true;
    // Which container to write. FLAC and Ogg need their libraries at build
    // time; `MakeAudioSink` returns nullptr for a container this build cannot
    // write and the export fails cleanly rather than writing the wrong file.
    AudioFileFormat container = AudioFileFormat::Wav;
    // Ogg Vorbis VBR quality in [0,1]; ignored by WAV and FLAC.
    float vorbisQuality = 0.5f;
};

// The timeline window to bounce, in PROJECT frames. The default is the whole
// project; `end < 0` means "to the project's own end", so an explicit
// `start` alone bounces from there to the end.
struct ExportRange {
    Frame start = 0;
    Frame end   = -1;
};

// Progress + cancellation for a long bounce. Both parts are optional.
//
// `progress` is called ON THE EXPORTING THREAD with a fraction in [0,1] that
// never decreases and reaches 1.0 exactly once, on success. It must be cheap
// and must not touch the model or any Haiku object — a UI hands in something
// that stores the value for its own looper to read.
//
// `cancel` is polled between blocks. When it reads true the export stops
// early, deletes what it had written (nothing lands at the destination path)
// and returns false.
struct ExportJob {
    std::function<void(float)> progress;
    const std::atomic<bool>*   cancel = nullptr;
};

// Loudness normalization / true-peak limiting of the finished master, to a
// target integrated loudness (ITU-R BS.1770 / EBU R128) and/or a ceiling
// (dBTP):
//   - `enabled` alone: gain-based normalization with true-peak safety — when
//     the target can't be reached without exceeding the ceiling, the gain is
//     backed off so the whole program lands below target rather than clipping.
//   - `limiter`: run a look-ahead true-peak limiter (see dsp/Limiter) to hold
//     the ceiling instead of backing the gain off. With normalization also on,
//     the program is pushed all the way to `targetLufs` and the limiter — not a
//     whole-mix attenuation — catches the peaks, so quiet material reaches
//     target loudness. `limiter` may be used without `enabled` to limit peaks
//     only. Either way the output true peak stays at/under `truePeakCeil`.
struct ExportNormalize {
    bool  enabled      = false;
    float targetLufs   = -14.0f;   // integrated LUFS target (e.g. -14 streaming)
    float truePeakCeil = -1.0f;    // dBTP ceiling the output must not exceed
    bool  limiter      = false;    // hold the ceiling with a look-ahead limiter
                                   // instead of gain-backoff (lets it hit target)
};

// Everything an export needs beyond the project and a destination. The default
// is the historical bounce: 16-bit dithered PCM, project rate, whole project,
// no normalization, no progress, no cancellation.
struct ExportOptions {
    ExportFormat     format{};
    ExportNormalize  normalize{};
    ExportRange      range{};
    const ExportJob* job = nullptr;
};

// Render `project` to a stereo file at `outPath` — WAV, FLAC or Ogg Vorbis,
// per opts.format.container — sampled at `outRate` Hz, in the format and
// window `opts` selects. If `outRate <= 0` the project's own sample rate is
// used.
//
// The timeline is laid out in *output* frames: a project-frame position p maps
// to output frame round((p - range.start) * outRate / project.sampleRate), so
// clip/note placement and fade lengths stay correct at any target rate and any
// window. Solo overrides mute (any soloed non-muted track mutes the rest).
// Returns false if there is nothing to render, the output file cannot be
// written (including "this build has no that format"), or the job's cancel
// flag was raised (in which case nothing is left at `outPath`: the file is
// written to `outPath.part` and renamed on success).
//
// Runs with no real-time constraints and may be called from any thread; it
// reads the project and never mutates it, so a caller that exports off the UI
// thread hands in a snapshot (see Project's copy semantics).
bool ExportWav(const Project& project, const std::string& outPath,
               double outRate = 0.0, const ExportOptions& opts = {});

// Bounce each non-bus track to its own stem file under `dir` (named
// "NN_<track>.<ext>", the extension of opts.format's container), each rendered
// through its own fader/fx/bus/master by
// soloing it, in the format/window `opts` selects. Progress spans the whole
// job (one stem is 1/N of it) and cancellation stops between stems and inside
// the one in flight. Returns the number of stems written -- a track with
// nothing to render contributes no stem but does not fail the run, and a
// cancelled run returns what it managed before the cancel (0 if none, which is
// what a caller should read as "cancelled or failed"). Kit-free, host-testable.
int ExportStems(const Project& project, const std::string& dir,
                double outRate = 0.0, const ExportOptions& opts = {});

} // namespace daw
