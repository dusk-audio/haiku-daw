#include "EffectFactory.h"

#include "Biquad.h"
#include "Delay.h"
#include "Reverb.h"
#include "Compressor.h"
#include "Eq.h"
#include "Saturator.h"
#include "Gate.h"
#include "Widener.h"

namespace daw {

static PluginFactoryFn gPluginFactory = nullptr;
void SetPluginFactory(PluginFactoryFn fn) { gPluginFactory = fn; }

std::unique_ptr<IEffect> MakeEffect(const EffectDesc& d) {
    if (d.type == EffectType::Plugin) {
        if (!gPluginFactory) return nullptr;
        auto e = gPluginFactory(d.pluginName);
        if (e)   // apply stored params
            for (size_t i = 0; i < d.params.size(); i++)
                e->SetParam((int)i, d.params[i]);
        return e;
    }
    switch (d.type) {
        case EffectType::Biquad: {
            Biquad::Type mode = Biquad::Type::LowPass;
            if (d.p(0) == 1.0f) mode = Biquad::Type::HighPass;
            else if (d.p(0) == 2.0f) mode = Biquad::Type::Peaking;
            return std::unique_ptr<IEffect>(
                new Biquad(mode, d.p(1), d.p(2), d.p(3)));
        }
        case EffectType::Delay:
            return std::unique_ptr<IEffect>(new Delay(d.p(0), d.p(1), d.p(2)));
        case EffectType::Reverb: {
            auto rv = std::unique_ptr<Reverb>(new Reverb(d.p(0), d.p(1)));
            // Apply all params (algorithm/decay/tone) before Prepare builds the
            // selected engine.
            for (size_t i = 0; i < d.params.size(); i++)
                rv->SetParam((int)i, d.params[i]);
            return rv;
        }
        case EffectType::Compressor:
            return std::unique_ptr<IEffect>(
                new Compressor(d.p(0), d.p(1), d.p(2), d.p(3), d.p(4)));
        case EffectType::Eq: {
            auto eq = std::unique_ptr<Eq>(new Eq());
            for (int b = 0; b < Eq::kBands; b++)
                eq->SetBand(b, d.p(b * 3 + 0), d.p(b * 3 + 1), d.p(b * 3 + 2));
            return eq;
        }
        case EffectType::Saturator:
            return std::unique_ptr<IEffect>(new Saturator(d.p(0), d.p(1), d.p(2)));
        case EffectType::Gate:
            return std::unique_ptr<IEffect>(
                new Gate(d.p(0), d.p(1), d.p(2), d.p(3), d.p(4)));
        case EffectType::Widener:
            return std::unique_ptr<IEffect>(new Widener(d.p(0), d.p(1), d.p(2)));
    }
    return nullptr;
}

} // namespace daw
