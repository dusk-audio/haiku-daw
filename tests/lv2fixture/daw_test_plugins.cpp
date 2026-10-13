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

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <cstdlib>
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

// How much of a latency MOVE the `latency-step` plugin makes, from its second
// run onward: it starts at zero latency (and reports zero) so that a host
// latching at activation is demonstrably wrong, then reports AND applies this.
//
// Real plugins do exactly this: 4K EQ 2 was measured reporting 0 at activate,
// 27 after one run, then 0 again once it decided its EQ was flat. Since M4.6
// the host OBSERVES such a move off the RT thread and re-solves the delay
// lines at the next graph build, so the fixture is honest — it reports what it
// applies, in both states — and the tests assert the host notices and adopts
// it. (stereo-latent used to report 999 while delaying 64, which was the right
// fixture for "latch once and never re-read"; under the new policy that plugin
// is simply lying, and believing the port is the policy.)
constexpr int kStepFrames = 128;

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
// It also writes the latency port on every run(), not just at activate, so a
// host that re-read it mid-run would see the value it has always been given.
// ---------------------------------------------------------------------------
struct StereoLatent {
    const float* in[2]   = {nullptr, nullptr};
    float*       out[2]  = {nullptr, nullptr};
    const float* gain    = nullptr;
    const float* extra   = nullptr;
    float*       latency = nullptr;
    std::vector<float> ring[2];
    size_t             pos = 0;
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
    // Published at activate, which is where a host latches it; re-published on
    // every run below. Reset() (host-side deactivate/activate) therefore puts
    // the truthful value back, which is what keeps a committed latency valid
    // across a seek.
    if (self->latency) *self->latency = (float)kLatentFrames;
}

void StereoLatentRun(LV2_Handle handle, uint32_t frames) {
    StereoLatent* self = static_cast<StereoLatent*>(handle);
    // Re-published every run: this is what the RT thread observes (M4.6), and
    // it never moves.
    if (self->latency) *self->latency = (float)kLatentFrames;
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
// urn:haiku-daw:test:latency-step — a plugin whose latency MOVES (M4.6).
//
// Its first run reports and applies 0 frames; from its second run on it
// reports and applies kStepFrames. Both halves matter: a host that reads the
// port straight after activation (or worse, keeps what it read there) sizes
// every delay line from a value the plugin is not using, while a host that
// observes the move and re-latches before the next build is exactly right.
//
// Honest in both states: the reported number is the delay actually applied.
// The transition itself is deliberately sharp — run 1 passes its input
// through while filling the ring, run 2 emits the ring, so 128 frames of the
// pre-transition input appear twice. Nothing in a steady-state test can see
// that, and it is what a plugin deciding "now I need 128 frames of lookahead"
// between two blocks really looks like.
// ---------------------------------------------------------------------------
struct LatencyStep {
    const float* in[2]   = {nullptr, nullptr};
    float*       out[2]  = {nullptr, nullptr};
    const float* gain    = nullptr;
    float*       latency = nullptr;
    std::vector<float> ring[2];
    size_t             pos     = 0;
    bool               engaged = false;   // has the step happened yet?
};

LV2_Handle LatencyStepInstantiate(const LV2_Descriptor*, double,
                                  const char*, const LV2_Feature* const*) {
    LatencyStep* self = new LatencyStep();
    for (int c = 0; c < 2; c++) self->ring[c].assign((size_t)kStepFrames, 0.0f);
    return self;
}

void LatencyStepConnect(LV2_Handle handle, uint32_t port, void* data) {
    LatencyStep* self = static_cast<LatencyStep*>(handle);
    switch (port) {
        case 0: self->in[0]   = static_cast<const float*>(data); break;
        case 1: self->in[1]   = static_cast<const float*>(data); break;
        case 2: self->out[0]  = static_cast<float*>(data);       break;
        case 3: self->out[1]  = static_cast<float*>(data);       break;
        case 4: self->gain    = static_cast<const float*>(data); break;
        case 5: self->latency = static_cast<float*>(data);       break;
        default: break;
    }
}

void LatencyStepActivate(LV2_Handle handle) {
    LatencyStep* self = static_cast<LatencyStep*>(handle);
    for (int c = 0; c < 2; c++)
        std::memset(self->ring[c].data(), 0, self->ring[c].size() * sizeof(float));
    self->pos     = 0;
    self->engaged = false;
    if (self->latency) *self->latency = 0.0f;
}

void LatencyStepRun(LV2_Handle handle, uint32_t frames) {
    LatencyStep* self = static_cast<LatencyStep*>(handle);
    if (self->latency)
        *self->latency = self->engaged ? (float)kStepFrames : 0.0f;
    if (!self->in[0] || !self->in[1] || !self->out[0] || !self->out[1]) return;

    const float g = self->gain ? *self->gain : 1.0f;
    for (uint32_t i = 0; i < frames; i++) {
        const size_t p = self->pos;
        for (int c = 0; c < 2; c++) {
            const float held = self->ring[c][p];
            self->ring[c][p] = self->in[c][i];
            // Run 1 is a passthrough that fills the ring; from run 2 the ring
            // IS the signal, kStepFrames late — which is what the port now
            // says.
            self->out[c][i] = (self->engaged ? held : self->in[c][i]) * g;
        }
        self->pos = (p + 1) % (size_t)kStepFrames;
    }
    self->engaged = true;   // from the NEXT run on, and it says so
}

void LatencyStepCleanup(LV2_Handle h) { delete static_cast<LatencyStep*>(h); }

// ---------------------------------------------------------------------------
// urn:haiku-daw:test:sidechain-gain — out = in * key: a keyed VCA.
//
// 2 required in / 2 required out plus a TWO-PORT OPTIONAL key pair, which is
// how LV2 spells "sidechain": the host may leave a connectionOptional input
// unconnected, so it is exactly the port a host may or may not feed a key.
// The DSP is one multiply, so a test can assert the key really arrived, on the
// right channel and at the right time — and with no key routed the key buffers
// are silence, which makes the output silence (the fail-soft this fixture
// demonstrates).
//
// A NULL key port would be the old host behaviour (optional ports were left
// unconnected before package 05's place for a signal existed). The guard below
// keeps that a clean silence rather than a crash, so the test reports a wrong
// value instead of taking the suite down.
// ---------------------------------------------------------------------------
struct SidechainGain {
    const float* in[2]  = {nullptr, nullptr};
    float*       out[2] = {nullptr, nullptr};
    const float* key[2] = {nullptr, nullptr};
    const float* gain   = nullptr;
};

LV2_Handle SidechainGainInstantiate(const LV2_Descriptor*, double,
                                    const char*, const LV2_Feature* const*) {
    return new SidechainGain();
}

void SidechainGainConnect(LV2_Handle handle, uint32_t port, void* data) {
    SidechainGain* self = static_cast<SidechainGain*>(handle);
    switch (port) {
        case 0: self->in[0]  = static_cast<const float*>(data); break;
        case 1: self->in[1]  = static_cast<const float*>(data); break;
        case 2: self->out[0] = static_cast<float*>(data);       break;
        case 3: self->out[1] = static_cast<float*>(data);       break;
        case 4: self->key[0] = static_cast<const float*>(data); break;
        case 5: self->key[1] = static_cast<const float*>(data); break;
        case 6: self->gain   = static_cast<const float*>(data); break;
        default: break;
    }
}

void SidechainGainRun(LV2_Handle handle, uint32_t frames) {
    SidechainGain* self = static_cast<SidechainGain*>(handle);
    if (!self->in[0] || !self->in[1] || !self->out[0] || !self->out[1]) return;
    const float g = self->gain ? *self->gain : 1.0f;
    for (uint32_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            const float k = self->key[c] ? self->key[c][i] : 0.0f;
            self->out[c][i] = self->in[c][i] * k * g;
        }
    }
}

void SidechainGainCleanup(LV2_Handle h) { delete static_cast<SidechainGain*>(h); }

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

// ---------------------------------------------------------------------------
// urn:haiku-daw:test:stateful — stereo gain with state that is NOT a port.
//
// This is the data-loss case the host's state feature exists for: `mul` is a
// value the plugin keeps internally, settable only through the state:interface
// (there is no control port for it), so the host can preserve it ONLY by asking
// the plugin for its state with lilv's state API and handing it back on the next
// instantiation. Output = in * gain * mul, so a test can assert the state
// actually reached the DSP rather than merely that a string was non-empty.
//
// The state property is keyed by URI, which is what the URID map feature the
// host passes into save()/restore() is for — a plugin that stored state by
// string key would not be exercising anything a real plugin does.
// ---------------------------------------------------------------------------
constexpr const char* kMulKeyUri = "urn:haiku-daw:test:state:mul";

struct Stateful {
    const float* in[2]  = {nullptr, nullptr};
    float*       out[2] = {nullptr, nullptr};
    const float* gain   = nullptr;
    float        mul    = 1.0f;
};

LV2_Handle StatefulInstantiate(const LV2_Descriptor*, double,
                               const char*, const LV2_Feature* const*) {
    return new Stateful();
}

void StatefulConnect(LV2_Handle handle, uint32_t port, void* data) {
    Stateful* self = static_cast<Stateful*>(handle);
    switch (port) {
        case 0: self->in[0]  = static_cast<const float*>(data); break;
        case 1: self->in[1]  = static_cast<const float*>(data); break;
        case 2: self->out[0] = static_cast<float*>(data);       break;
        case 3: self->out[1] = static_cast<float*>(data);       break;
        case 4: self->gain   = static_cast<const float*>(data); break;
        default: break;
    }
}

void StatefulRun(LV2_Handle handle, uint32_t frames) {
    Stateful* self = static_cast<Stateful*>(handle);
    if (!self->in[0] || !self->in[1] || !self->out[0] || !self->out[1]) return;
    const float k = (self->gain ? *self->gain : 1.0f) * self->mul;
    for (uint32_t i = 0; i < frames; i++) {
        self->out[0][i] = self->in[0][i] * k;
        self->out[1][i] = self->in[1][i] * k;
    }
}

void StatefulCleanup(LV2_Handle h) { delete static_cast<Stateful*>(h); }

// The features the host hands in, and nothing else: no map, no state.
const LV2_URID_Map* MapOf(const LV2_Feature* const* features) {
    if (!features) return nullptr;
    for (int i = 0; features[i]; i++)
        if (features[i]->URI && !std::strcmp(features[i]->URI, LV2_URID__map))
            return static_cast<const LV2_URID_Map*>(features[i]->data);
    return nullptr;
}

LV2_State_Status StatefulSave(LV2_Handle handle, LV2_State_Store_Function store,
                              LV2_State_Handle state, uint32_t,
                              const LV2_Feature* const* features) {
    Stateful* self = static_cast<Stateful*>(handle);
    const LV2_URID_Map* map = MapOf(features);
    if (!map) return LV2_STATE_ERR_NO_FEATURE;   // the host must pass it
    const LV2_URID key = map->map(map->handle, kMulKeyUri);
    const LV2_URID type = map->map(map->handle, LV2_ATOM__Float);
    return store(state, key, &self->mul, sizeof(float), type,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status StatefulRestore(LV2_Handle handle,
                                 LV2_State_Retrieve_Function retrieve,
                                 LV2_State_Handle state, uint32_t,
                                 const LV2_Feature* const* features) {
    Stateful* self = static_cast<Stateful*>(handle);
    const LV2_URID_Map* map = MapOf(features);
    if (!map) return LV2_STATE_ERR_NO_FEATURE;
    const LV2_URID key = map->map(map->handle, kMulKeyUri);

    size_t   size  = 0;
    uint32_t type  = 0, flags = 0;
    const void* value = retrieve(state, key, &size, &type, &flags);
    if (!value) return LV2_STATE_SUCCESS;   // not in this state: keep as is
    // A float is what we write; a document authored elsewhere (a hand-written
    // preset, another host, another version) may carry the same property as a
    // double, an int, or a plain literal — lilv hands a non-typed property over
    // as its TEXT — and all of them mean the same number.
    const LV2_URID atomFloat  = map->map(map->handle, LV2_ATOM__Float);
    const LV2_URID atomDouble = map->map(map->handle, LV2_ATOM__Double);
    const LV2_URID atomInt    = map->map(map->handle, LV2_ATOM__Int);
    const LV2_URID atomLong   = map->map(map->handle, LV2_ATOM__Long);
    if (type == atomFloat && size == sizeof(float)) {
        self->mul = *(const float*)value;
    } else if (type == atomDouble && size == sizeof(double)) {
        self->mul = (float)*(const double*)value;
    } else if (type == atomInt && size == sizeof(int32_t)) {
        self->mul = (float)*(const int32_t*)value;
    } else if (type == atomLong && size == sizeof(int64_t)) {
        self->mul = (float)*(const int64_t*)value;
    } else if (size > 0 && size < 64) {
        char buf[64];
        std::memcpy(buf, value, size);
        buf[size] = '\0';
        char* end = nullptr;
        const double parsed = std::strtod(buf, &end);
        if (end != buf) self->mul = (float)parsed;
    }
    return LV2_STATE_SUCCESS;
}

const void* StatefulExtension(const char* uri) {
    static const LV2_State_Interface iface = { StatefulSave, StatefulRestore };
    if (uri && !std::strcmp(uri, LV2_STATE__interface)) return &iface;
    return nullptr;
}

const LV2_Descriptor kDescriptors[] = {
    { "urn:haiku-daw:test:mono-gain",
      MonoGainInstantiate, MonoGainConnect, nullptr,
      MonoGainRun, nullptr, MonoGainCleanup, nullptr },
    { "urn:haiku-daw:test:stateful",
      StatefulInstantiate, StatefulConnect, nullptr,
      StatefulRun, nullptr, StatefulCleanup, StatefulExtension },
    { "urn:haiku-daw:test:stereo-latent",
      StereoLatentInstantiate, StereoLatentConnect, StereoLatentActivate,
      StereoLatentRun, nullptr, StereoLatentCleanup, nullptr },
    { "urn:haiku-daw:test:latency-step",
      LatencyStepInstantiate, LatencyStepConnect, LatencyStepActivate,
      LatencyStepRun, nullptr, LatencyStepCleanup, nullptr },
    { "urn:haiku-daw:test:sidechain-gain",
      SidechainGainInstantiate, SidechainGainConnect, nullptr,
      SidechainGainRun, nullptr, SidechainGainCleanup, nullptr },
    // Shares the mono-gain body: this plugin exists for its RDF -- an
    // lv2:enumeration whose scale points carry rdfs:labels, and a port name
    // that declares its unit ("Frequency (Hz)") -- but it still has to
    // INSTANTIATE, because the listing's whole promise is that every plugin on
    // it does. Its extra control ports are simply never read.
    { "urn:haiku-daw:test:filter-unit",
      MonoGainInstantiate, MonoGainConnect, nullptr,
      MonoGainRun, nullptr, MonoGainCleanup, nullptr },
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
