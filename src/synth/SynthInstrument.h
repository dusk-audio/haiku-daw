// SynthInstrument — the built-in oscillator voice behind the IInstrument
// interface.
//
// A thin adapter: it owns a Synth and the Instrument parameters, so the engine
// can treat the stock voice and a sampler identically. Rendering is byte-for-
// byte what Engine used to do by calling fSynth.Render(notes, b.instrument, ...)
// directly, so existing projects sound exactly the same.
//
// Kit-free, host-testable.
#pragma once

#include "IInstrument.h"
#include "Synth.h"
#include "../model/Instrument.h"

namespace daw {

class SynthInstrument : public IInstrument {
public:
    explicit SynthInstrument(const Instrument& inst, double sampleRate = 48000.0)
        : fInst(inst), fSynth(sampleRate) {}

    void Prepare(double sampleRate) override { fSynth.SetSampleRate(sampleRate); }

    void Render(const std::vector<MidiNote>& notes,
                float* out, size_t frames, Frame blockStart,
                StereoGain from, StereoGain to,
                const VoiceExpression& expr = {}) override {
        fSynth.Render(notes, fInst, out, frames, blockStart, from, to, expr);
    }

    using IInstrument::Render;   // keep the constant-gain convenience overloads

    const char* Name() const override { return "Synth"; }

private:
    Instrument fInst;
    Synth      fSynth;
};

} // namespace daw
