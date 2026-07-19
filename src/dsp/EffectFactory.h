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

// The same hook for LV2 plugins, installed by the (host-side) LV2 layer so this
// factory never links lilv. `desc.pluginName` carries the plugin URI, and the
// whole descriptor is passed because an LV2 instance is bound to a concrete
// sample rate at instantiation (unlike the native add-ons, which are configured
// in Prepare). Returns an unprepared IEffect the CALLER OWNS — MakeEffect wraps
// it in the unique_ptr it hands back — or nullptr if the URI can't be resolved.
// A null hook or a null return degrades exactly like an unavailable native
// plugin: MakeEffect yields nullptr and the chain keeps an index-aligned hole.
using Lv2FactoryFn = IEffect* (*)(const EffectDesc& desc, double sampleRate);
void SetLv2Factory(Lv2FactoryFn fn);

// Returns nullptr for an unknown/unavailable type. The caller Prepare()s the
// effect with the target sample rate before use. Plugin and LV2 effects also get
// their stored params applied via SetParam. `sampleRate` is the rate the caller
// will Prepare() at; it is only consulted by the LV2 hook (which must
// instantiate at a concrete rate). 0 = unknown/don't care.
std::unique_ptr<IEffect> MakeEffect(const EffectDesc& desc,
                                    double sampleRate = 0.0);

} // namespace daw
