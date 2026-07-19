// daw_test_plugins — a tiny LV2 bundle built by this repo, purely as a test
// fixture for the LV2 host.
//
// Why this exists: until now lv2_host_tests could only assert INVARIANTS over
// whatever plugins a machine happened to have installed, because the two
// development machines have completely different sets and CI would have neither.
// That left real gaps that no amount of invariant-checking could close:
//
//   - the MonoDual (1-in/1-out, two-instance) path ran only on Haiku, and only
//     because Haiku's lv2 package happens to ship mono example plugins;
//   - every installed plugin reports ZERO latency, so every latency assertion in
//     the suite was vacuously true and the latching logic was never really
//     exercised;
//   - the rejection paths depended on a specific broken bundle and a specific
//     sfizz install being present.
//
// These five plugins are deliberately trivial DSP with exactly-known behaviour,
// so the tests can assert on VALUES (this sample equals that one, latency is
// exactly 64) instead of on properties. They are never installed anywhere; the
// build points LV2_PATH at the build tree.
//
// Kit-free C++ against the LV2 headers only, so it builds wherever the host does.

#include <lv2/core/lv2.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

// Reported AND actually-applied latency of the latent plugin, in frames. The
// tests assert both halves: that the host reads 64 out of the latency port, and
// that the audio really does come out 64 frames late. A plugin that reported a
// latency it did not apply would sail through a test that only checked the
// number.
constexpr int kLatentFrames = 64;

// After its first run(), the latent plugin starts REPORTING this instead, while
// still delaying by kLatentFrames. It is lying on purpose.
//
// Real plugins do exactly this: 4K EQ 2 was measured reporting 0 at activate,
// 27 after one run, then 0 again once it decided its EQ was flat. IEffect
// requires LatencySamples() to be constant for a whole Prepare/Process lifetime,
// because PDC and RunInsertSlot's per-insert dry-delay line are both sized from
// it once. Without a plugin that MOVES the value, "the host latches it and never
// re-reads" is untestable — every assertion passes trivially against a plugin
// that always reports the same number. This is what gives that rule teeth.
constexpr int kLatentLieFrames = 999;

// ---------------------------------------------------------------------------
// urn:haiku-daw:test:mono-gain — 1 in, 1 out. Hosted as TWO instances, one per
// channel (Lv2Topology::MonoDual). Its gain is deliberately a plain multiply so
// a test can prove each instance was fed ITS OWN channel: feed L and R different
// values and the outputs must stay different and correctly scaled. That is the
// per-channel routing bug this topology invites, and nothing else catches it.
// ---------------------------------------------------------------------------
struct MonoGain {
    const float* in   = nullptr;
    float*       out  = nullptr;
    const float* gain = nullptr;
};

LV2_Handle MonoGainInstantiate(const LV2_Descriptor*, double,
                               const char*, const LV2_Feature* const*) {
    return new MonoGain();
}

void MonoGainConnect(LV2_Handle handle, uint32_t port, void* data) {
    MonoGain* self = static_cast<MonoGain*>(handle);
    switch (port) {
        case 0: self->in   = static_cast<const float*>(data); break;
        case 1: self->out  = static_cast<float*>(data);       break;
        case 2: self->gain = static_cast<const float*>(data); break;
        default: break;
    }
}

void MonoGainRun(LV2_Handle handle, uint32_t frames) {
    MonoGain* self = static_cast<MonoGain*>(handle);
    if (!self->in || !self->out) return;
    const float g = self->gain ? *self->gain : 1.0f;
    for (uint32_t i = 0; i < frames; i++) self->out[i] = self->in[i] * g;
}

void MonoGainCleanup(LV2_Handle handle) { delete static_cast<MonoGain*>(handle); }

// ---------------------------------------------------------------------------
// urn:haiku-daw:test:stereo-latent — 2 in, 2 out, and the only plugin available
// to this project that reports a NON-ZERO latency. It delays by exactly
// kLatentFrames and reports exactly that through an output control port carrying
// `lv2:designation lv2:latency`.
//
// It also writes the latency port on every run(), not just at activate, so it
// exercises the host's "latch it once and never let it move" rule against a
// plugin that keeps re-publishing the value.
// ---------------------------------------------------------------------------
struct StereoLatent {
    const float* in[2]   = {nullptr, nullptr};
    float*       out[2]  = {nullptr, nullptr};
    const float* gain    = nullptr;
    const float* extra   = nullptr;
    float*       latency = nullptr;
    std::vector<float> ring[2];
    size_t             pos = 0;
    bool               hasRun = false;
};

LV2_Handle StereoLatentInstantiate(const LV2_Descriptor*, double,
                                   const char*, const LV2_Feature* const*) {
    StereoLatent* self = new StereoLatent();
    for (int c = 0; c < 2; c++) self->ring[c].assign((size_t)kLatentFrames, 0.0f);
    return self;
}

void StereoLatentConnect(LV2_Handle handle, uint32_t port, void* data) {
    StereoLatent* self = static_cast<StereoLatent*>(handle);
    switch (port) {
        case 0: self->in[0]   = static_cast<const float*>(data); break;
        case 1: self->in[1]   = static_cast<const float*>(data); break;
        case 2: self->out[0]  = static_cast<float*>(data);       break;
        case 3: self->out[1]  = static_cast<float*>(data);       break;
        case 4: self->gain    = static_cast<const float*>(data); break;
        case 5: self->extra   = static_cast<const float*>(data); break;
        case 6: self->latency = static_cast<float*>(data);       break;
        default: break;
    }
}

void StereoLatentActivate(LV2_Handle handle) {
    StereoLatent* self = static_cast<StereoLatent*>(handle);
    for (int c = 0; c < 2; c++)
        std::memset(self->ring[c].data(), 0, self->ring[c].size() * sizeof(float));
    self->pos = 0;
    self->hasRun = false;
    // Published at activate, which is where the host latches it. Note that
    // deactivate/activate (the host's Reset) puts the truthful value back, so a
    // host that re-latched on Reset would silently look correct again -- the
    // test therefore checks latency AFTER a Process, not only after Reset.
    if (self->latency) *self->latency = (float)kLatentFrames;
}

void StereoLatentRun(LV2_Handle handle, uint32_t frames) {
    StereoLatent* self = static_cast<StereoLatent*>(handle);
    // From the second run onward, report a value that is flatly wrong while the
    // actual delay stays kLatentFrames. Any host that re-reads this port after
    // Prepare will pick up the lie and be caught.
    if (self->latency)
        *self->latency = self->hasRun ? (float)kLatentLieFrames
                                      : (float)kLatentFrames;
    self->hasRun = true;
    if (!self->in[0] || !self->in[1] || !self->out[0] || !self->out[1]) return;

    const float g = self->gain ? *self->gain : 1.0f;
    for (uint32_t i = 0; i < frames; i++) {
        const size_t p = self->pos;
        for (int c = 0; c < 2; c++) {
            const float delayed = self->ring[c][p];
            self->ring[c][p]    = self->in[c][i];
            self->out[c][i]     = delayed * g;
        }
        self->pos = (p + 1) % (size_t)kLatentFrames;
    }
}

void StereoLatentCleanup(LV2_Handle h) { delete static_cast<StereoLatent*>(h); }

// ---------------------------------------------------------------------------
// Two plugins that MUST be refused, so the rejection paths are tested without
// depending on a particular broken bundle being installed:
//   bad-topology  — 3 audio in / 1 audio out, none optional.
//   needs-feature — fine topology, but requires worker:schedule (declared in the
//                   TTL), which this host does not implement.
//   cv-port       — perfect 2/2 audio topology, plus a MANDATORY CV input. A CV
//                   port carries one float per frame, so the small inert buffer
//                   that satisfies an atom port would be overrun by a whole
//                   block. The host has no correctly-sized buffer to offer and
//                   must decline the plugin rather than host it unsafely.
// Both still provide working entry points: if the host ever wrongly accepts one,
// the failure should be a clean assertion, not a crash in the fixture.
// ---------------------------------------------------------------------------
struct Inert {
    std::vector<float*> ports;
};

LV2_Handle InertInstantiate(const LV2_Descriptor*, double,
                            const char*, const LV2_Feature* const*) {
    return new Inert();
}

void InertConnect(LV2_Handle handle, uint32_t port, void* data) {
    Inert* self = static_cast<Inert*>(handle);
    if (self->ports.size() <= port) self->ports.resize(port + 1, nullptr);
    self->ports[port] = static_cast<float*>(data);
}

void InertRun(LV2_Handle, uint32_t) {}
void InertCleanup(LV2_Handle h) { delete static_cast<Inert*>(h); }

const LV2_Descriptor kDescriptors[] = {
    { "urn:haiku-daw:test:mono-gain",
      MonoGainInstantiate, MonoGainConnect, nullptr,
      MonoGainRun, nullptr, MonoGainCleanup, nullptr },
    { "urn:haiku-daw:test:stereo-latent",
      StereoLatentInstantiate, StereoLatentConnect, StereoLatentActivate,
      StereoLatentRun, nullptr, StereoLatentCleanup, nullptr },
    { "urn:haiku-daw:test:bad-topology",
      InertInstantiate, InertConnect, nullptr,
      InertRun, nullptr, InertCleanup, nullptr },
    { "urn:haiku-daw:test:needs-feature",
      InertInstantiate, InertConnect, nullptr,
      InertRun, nullptr, InertCleanup, nullptr },
    { "urn:haiku-daw:test:cv-port",
      InertInstantiate, InertConnect, nullptr,
      InertRun, nullptr, InertCleanup, nullptr },
};

} // namespace

extern "C" __attribute__((visibility("default")))
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    const uint32_t count = sizeof(kDescriptors) / sizeof(kDescriptors[0]);
    return index < count ? &kDescriptors[index] : nullptr;
}
