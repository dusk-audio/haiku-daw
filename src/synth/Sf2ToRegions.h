// Sf2ToRegions — turn one SF2 preset into a playable LoadedInstrument.
//
// Adapted from DuskStudio's Sf2ToSfz, which implements the SoundFont 2.04
// articulation model: four levels of generator layering (preset-global ->
// preset-zone -> instrument-global -> instrument-zone), key/velocity ranges
// that INTERSECT across levels while tune/attenuation/pan ADD, timecent and
// centibel unit conversions, and exclusiveClass -> choke group.
//
// The difference from DuskStudio: it emitted SFZ text plus extracted WAVs on
// disk, because sfizz would only accept a file. We own the sampler, so regions
// and PCM are built straight into memory — no temp files, no re-parse, and no
// locale hazard in the round trip.
//
// Not carried over: the lowpass filter (cutoff/resonance) generators, since the
// sampler has no filter yet. Affected presets still play, just unfiltered.
//
// Kit-free, host-testable.
#pragma once

#include "SampleBank.h"

#include <string>
#include <vector>

namespace daw {

// One selectable preset of an SF2, for the UI's preset list.
struct Sf2PresetInfo {
    std::string name;
    int         index   = 0;   // index into the file's preset list
    int         bank    = 0;
    int         program = 0;
};

// List an SF2's presets without decoding any audio. Cheap: metadata only.
// Returns an empty vector (and sets *error) if the file will not parse.
std::vector<Sf2PresetInfo> ListSf2Presets(const std::string& path,
                                          std::string* error);

// Convert preset `presetIndex` of an .sf2 into `out`. Decodes every sample the
// preset references. Slow — off the realtime thread only. Calls
// FinalizeInstrument before returning. Returns false and sets *error on
// failure; `presetIndex` is clamped into range rather than rejected.
// `warning` (optional) receives a PARTIAL-SUCCESS note: regions were produced
// but some were dropped, e.g. the preset exceeding kMaxInstrumentBytes. A true
// return with a warning set means "usable, but incomplete"; false means nothing
// usable and `error` says why.
bool LoadSf2Preset(const std::string& path, int presetIndex,
                   LoadedInstrument* out, std::string* error,
                   std::string* warning = nullptr);

} // namespace daw
