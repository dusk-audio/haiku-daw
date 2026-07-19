#include "InstrumentFactory.h"

#include "SampleBank.h"
#include "Sampler.h"
#include "SynthInstrument.h"

namespace daw {

std::unique_ptr<IInstrument> MakeInstrument(const InstrumentDesc& desc,
                                            double sampleRate,
                                            const SoundfontCache& cache) {
    switch (desc.type) {
        case InstrumentType::Sfz:
        case InstrumentType::Sf2: {
            // Look up only — never load. See the header for why.
            LoadedInstrumentPtr inst = cache.Get(desc.path, desc.sf2Preset);
            if (inst && !inst->regions.empty())
                return std::make_unique<Sampler>(std::move(inst), sampleRate);
            break;   // fall through to the synth voice
        }
        case InstrumentType::Synth:
        default:
            break;
    }
    return std::make_unique<SynthInstrument>(desc.synth, sampleRate);
}

std::unique_ptr<IInstrument> MakeInstrument(const InstrumentDesc& desc,
                                            double sampleRate) {
    return MakeInstrument(desc, sampleRate, SoundfontCache::Instance());
}

} // namespace daw
