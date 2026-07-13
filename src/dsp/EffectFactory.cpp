#include "EffectFactory.h"

#include "Biquad.h"
#include "Delay.h"
#include "Reverb.h"
#include "Compressor.h"

namespace daw {

std::unique_ptr<IEffect> MakeEffect(const EffectDesc& d) {
    switch (d.type) {
        case EffectType::Biquad: {
            Biquad::Type mode = Biquad::Type::LowPass;
            if (d.p0 == 1.0f) mode = Biquad::Type::HighPass;
            else if (d.p0 == 2.0f) mode = Biquad::Type::Peaking;
            return std::unique_ptr<IEffect>(
                new Biquad(mode, d.p1, d.p2, d.p3));
        }
        case EffectType::Delay:
            return std::unique_ptr<IEffect>(new Delay(d.p0, d.p1, d.p2));
        case EffectType::Reverb:
            return std::unique_ptr<IEffect>(new Reverb(d.p0, d.p1));
        case EffectType::Compressor:
            return std::unique_ptr<IEffect>(
                new Compressor(d.p0, d.p1, d.p2, d.p3, d.p4));
    }
    return nullptr;
}

} // namespace daw
