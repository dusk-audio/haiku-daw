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

// Does an insert built from `desc` consume an external sidechain key? The
// built-in types answer from model/Effect.h's EffectSupportsSidechain; an LV2
// plugin's answer is a property of the PLUGIN (its topology has optional audio
// inputs, i.e. a sidechain pair), which only the plugin layer can know — so it
// installs a probe here, exactly as it installs the factory. A null probe (no
// LV2 in this build, or nothing scanned yet) answers "no", which is the same
// degradation an unavailable plugin type takes: no key is routed and the editor
// offers no source.
using Lv2SidechainProbeFn = bool (*)(const std::string& uri);
void SetLv2SidechainProbe(Lv2SidechainProbeFn fn);

// True when the engine/Exporter must resolve, tap and PDC-align a key for this
// insert, and when the editor must offer the source picker. ONE predicate, so
// routing and the UI can never disagree about who takes a key.
bool EffectTakesSidechain(const EffectDesc& desc);

// Returns nullptr for an unknown/unavailable type. The caller Prepare()s the
// effect with the target sample rate before use. Plugin and LV2 effects also get
// their stored params applied via SetParam. `sampleRate` is the rate the caller
// will Prepare() at; it is only consulted by the LV2 hook (which must
// instantiate at a concrete rate). 0 = unknown/don't care.
std::unique_ptr<IEffect> MakeEffect(const EffectDesc& desc,
                                    double sampleRate = 0.0);

} // namespace daw
