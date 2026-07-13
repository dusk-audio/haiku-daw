#include "EffectFactory.h"

#include "Biquad.h"
#include "Delay.h"
#include "Reverb.h"
#include "Compressor.h"
#include "Eq.h"

namespace daw {

std::unique_ptr<IEffect> MakeEffect(const EffectDesc& d) {
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
        case EffectType::Reverb:
            return std::unique_ptr<IEffect>(new Reverb(d.p(0), d.p(1)));
        case EffectType::Compressor:
            return std::unique_ptr<IEffect>(
                new Compressor(d.p(0), d.p(1), d.p(2), d.p(3), d.p(4)));
        case EffectType::Eq: {
            auto eq = std::unique_ptr<Eq>(new Eq());
            for (int b = 0; b < Eq::kBands; b++)
                eq->SetBand(b, d.p(b * 3 + 0), d.p(b * 3 + 1), d.p(b * 3 + 2));
            return eq;
        }
    }
    return nullptr;
}

} // namespace daw
