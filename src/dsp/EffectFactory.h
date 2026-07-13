// EffectFactory — instantiate a live IEffect from a model EffectDesc.
//
// The one place that maps serializable descriptors to concrete DSP objects, so
// the engine (and host tests) build effect chains without knowing each type's
// constructor. Kit-free.
#pragma once

#include "IEffect.h"
#include "../model/Effect.h"

#include <memory>

namespace daw {

// Returns nullptr for an unknown type. The caller Prepare()s the effect with
// the target sample rate before use.
std::unique_ptr<IEffect> MakeEffect(const EffectDesc& desc);

} // namespace daw
