// InstrumentFactory — InstrumentDesc -> IInstrument.
//
// The instrument-side counterpart of dsp/EffectFactory.h, and deliberately the
// same shape: a plain switch, called off the realtime thread when the engine
// builds its graph.
//
// Soundfont instruments are NOT loaded here. The factory only looks the file up
// in the SoundfontCache; loading it is the UI's job, done once when the user
// picks the file or opens the project. That split is what makes a loop-record
// restart cheap — the engine rebuilds every Bus at the loop seam, and a decode
// there would stall the audio thread every time around the loop.
//
// A soundfont that is not in the cache (missing file, moved sample folder,
// project opened before the load finished) falls back to the built-in synth
// voice rather than to silence, so a track still plays and the problem is
// audible rather than mysterious.
//
// Kit-free, host-testable.
#pragma once

#include "IInstrument.h"
#include "../model/Instrument.h"

#include <memory>

namespace daw {

class SoundfontCache;

std::unique_ptr<IInstrument> MakeInstrument(const InstrumentDesc& desc,
                                            double sampleRate,
                                            const SoundfontCache& cache);

// Convenience for callers with no cache of their own (tests, and any path that
// only ever uses the built-in synth): uses SoundfontCache::Instance().
std::unique_ptr<IInstrument> MakeInstrument(const InstrumentDesc& desc,
                                            double sampleRate);

} // namespace daw
