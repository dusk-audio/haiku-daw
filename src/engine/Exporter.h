// Exporter — offline "bounce" of a Project to a stereo WAV file (PCM 16/24-bit
// or 32-bit IEEE float, selected per export).
//
// Renders the whole timeline to disk with no real-time constraints: no
// BSoundPlayer, no ring buffers, no disk threads. It reuses the same kit-free
// building blocks the live Engine uses (WavSource, Resampler, Synth,
// EffectFactory, WavWriter) and replicates the Engine's mix math — equal-power
// pan, per-track fx chain, master gain — so an export sounds like playback.
//
// Everything is done in-RAM: offline rendering values simplicity and exactness
// over memory footprint. Kit-free (STL + the project's own classes only) so it
// builds and unit-tests on any host.
#pragma once

#include "../model/Project.h"

#include <string>

namespace daw {

// Render `project` to a stereo WAV at `outPath`, sampled at `outRate` Hz, in the
// format chosen by `bitDepth` (see below). If `outRate <= 0` the project's own
// sample rate is used.
//
// The timeline is laid out in *output* frames: a project-frame position p maps
// to output frame round(p * outRate / project.sampleRate), so clip/note
// placement and fade lengths stay correct at any target rate. Solo overrides
// mute (any soloed non-muted track mutes the rest). Returns false if there is
// nothing to render or the output file cannot be written.
//
// `bitDepth` selects the output sample format: 16 or 24 = PCM (16-bit is
// TPDF-dithered), 32 = IEEE float. Any other value falls back to 16.
//
// `norm` optionally loudness-normalizes the finished master to a target
// integrated loudness (ITU-R BS.1770 / EBU R128) and/or true-peak-limits it to
// a ceiling (dBTP):
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

bool ExportWav(const Project& project, const std::string& outPath,
               double outRate = 0.0, int bitDepth = 16,
               ExportNormalize norm = {});

// Bounce each non-bus track to its own WAV stem under `dir` (named
// "NN_<track>.wav"), each rendered through its own fader/fx/bus/master by
// soloing it. Returns the number of stems written. Kit-free, host-testable.
int ExportStems(const Project& project, const std::string& dir,
                double outRate = 0.0);

} // namespace daw
