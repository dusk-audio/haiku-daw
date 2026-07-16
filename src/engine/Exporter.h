// Exporter — offline "bounce" of a Project to a 16-bit stereo WAV file.
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

// Render `project` to a 16-bit PCM stereo WAV at `outPath`, sampled at
// `outRate` Hz. If `outRate <= 0` the project's own sample rate is used.
//
// The timeline is laid out in *output* frames: a project-frame position p maps
// to output frame round(p * outRate / project.sampleRate), so clip/note
// placement and fade lengths stay correct at any target rate. Solo overrides
// mute (any soloed non-muted track mutes the rest). Returns false if there is
// nothing to render or the output file cannot be written.
bool ExportWav(const Project& project, const std::string& outPath,
               double outRate = 0.0);

// Bounce each non-bus track to its own WAV stem under `dir` (named
// "NN_<track>.wav"), each rendered through its own fader/fx/bus/master by
// soloing it. Returns the number of stems written. Kit-free, host-testable.
int ExportStems(const Project& project, const std::string& dir,
                double outRate = 0.0);

} // namespace daw
