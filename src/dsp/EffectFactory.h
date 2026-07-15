// EffectFactory — instantiate a live IEffect from a model EffectDesc.
//
// The one place that maps serializable descriptors to concrete DSP objects, so
// the engine (and host tests) build effect chains without knowing each type's
// constructor. Kit-free.
#pragma once

#include "IEffect.h"
#include "../model/Effect.h"

#include <memory>
#include <string>

namespace daw {

// A hook the Haiku layer installs so MakeEffect can create plugin (add-on)
// effects while this factory stays kit-free (null on host -> plugins skipped).
// Returns an unprepared IEffect for the named plugin, or nullptr.
using PluginFactoryFn = std::unique_ptr<IEffect> (*)(const std::string& name);
void SetPluginFactory(PluginFactoryFn fn);

// Returns nullptr for an unknown/unavailable type. The caller Prepare()s the
// effect with the target sample rate before use. Plugin effects also get their
// stored params applied via SetParam.
std::unique_ptr<IEffect> MakeEffect(const EffectDesc& desc);

} // namespace daw
