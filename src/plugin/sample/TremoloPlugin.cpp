// TremoloPlugin — a sample native effect add-on for the DAW.
//
// Demonstrates the plugin ABI (see src/plugin/PluginApi.h): implement
// daw::IEffect and export the four C factory symbols. Built as a Haiku add-on
// (.so) and dropped into the DAW's plugin dir; the host loads it with
// load_add_on() and hosts it like a built-in effect.
//
// Tremolo: amplitude modulation by a sine LFO. Two params — rate (Hz) and
// depth [0,1]. RT-safe: Process/SetParam only touch preallocated state.
#include "../PluginApi.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979323846f;

class Tremolo : public daw::IEffect {
public:
    explicit Tremolo(double sampleRate) { Prepare(sampleRate); }

    void Prepare(double sampleRate) override {
        fSampleRate = sampleRate > 0.0 ? (float)sampleRate : 44100.0f;
        UpdateInc();
    }

    void Process(float* stereo, int frames) override {
        for (int i = 0; i < frames; i++) {
            // Sine LFO in [0,1]; depth scales how much it dips toward silence.
            float lfo = 0.5f * (1.0f + std::sin(fPhase));
            float gain = 1.0f - fDepth * lfo;
            stereo[2 * i]     *= gain;
            stereo[2 * i + 1] *= gain;
            fPhase += fInc;
            if (fPhase >= 2.0f * kPi) fPhase -= 2.0f * kPi;
        }
    }

    void Reset() override { fPhase = 0.0f; }

    void SetParam(int slot, float value) override {
        if (slot == 0) { fRateHz = value; UpdateInc(); }
        else if (slot == 1) { fDepth = value < 0 ? 0 : (value > 1 ? 1 : value); }
    }

    const char* Name() const override { return "Tremolo"; }

private:
    void UpdateInc() { fInc = 2.0f * kPi * fRateHz / fSampleRate; }

    float fSampleRate = 44100.0f;
    float fRateHz = 5.0f;
    float fDepth = 0.5f;
    float fPhase = 0.0f;
    float fInc = 0.0f;
};

}  // namespace

extern "C" {

const char* daw_plugin_name() { return "Tremolo"; }

int daw_plugin_param_count() { return 2; }

void daw_plugin_param_info(int i, const char** name, float* mn, float* mx,
                           float* def) {
    struct { const char* n; float mn, mx, def; } kParams[] = {
        {"Rate",  0.1f, 20.0f, 5.0f},
        {"Depth", 0.0f, 1.0f,  0.5f},
    };
    if (i < 0 || i >= 2) i = 0;
    if (name) *name = kParams[i].n;
    if (mn)   *mn   = kParams[i].mn;
    if (mx)   *mx   = kParams[i].mx;
    if (def)  *def  = kParams[i].def;
}

daw::IEffect* daw_plugin_create(double sampleRate) {
    return new Tremolo(sampleRate);
}

}  // extern "C"
