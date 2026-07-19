#include "Lv2Host.h"

#include "Lv2PortMap.h"
#include "../dsp/EffectFactory.h"
#include "../model/Effect.h"

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/urid/urid.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace daw {
namespace {

// Bytes of atom scratch per atom port. Also advertised as bufsz:sequenceSize, so
// a plugin that asks how much event space it has gets a truthful answer. We
// neither send nor read events in v1 — these buffers exist because the LV2 spec
// requires every port to be connected, and an atom port is the one non-audio
// port class whose buffer size the HOST gets to choose.
constexpr uint32_t kAtomBufBytes = 8192;
constexpr size_t   kAtomBufWords = kAtomBufBytes / sizeof(uint64_t);

// Rewrite an atom port buffer's header so the plugin sees a well-formed sequence.
// Cheap enough (four stores) to redo before every run, which is what keeps this
// correct: an input buffer must be re-emptied in case the plugin wrote to it,
// and an output buffer must be re-told its capacity, which the plugin overwrites
// with the size it actually produced.
void ResetAtomSequence(uint64_t* buf, LV2_URID sequenceType, bool output) {
    LV2_Atom_Sequence* s = reinterpret_cast<LV2_Atom_Sequence*>(buf);
    s->atom.type = sequenceType;
    s->atom.size = output ? (uint32_t)(kAtomBufBytes - sizeof(LV2_Atom))
                          : (uint32_t)sizeof(LV2_Atom_Sequence_Body);
    s->body.unit = 0;
    s->body.pad  = 0;
}

// urid:map / urid:unmap over a plain string table.
//
// Called during instantiation (off the RT thread) by essentially every modern
// plugin. The mutex guards the case of two effects instantiating concurrently —
// it is never taken on the audio path. A plugin that calls map() from run()
// would allocate under the RT thread; that is a known-bad plugin behaviour we do
// not defend against beyond noting it.
class UridMap {
public:
    UridMap() {
        fMap.handle   = this;  fMap.map     = &MapCb;
        fUnmap.handle = this;  fUnmap.unmap = &UnmapCb;
        fMapFeature   = { LV2_URID__map,   &fMap };
        fUnmapFeature = { LV2_URID__unmap, &fUnmap };
    }

    LV2_URID Map(const char* uri) {
        if (!uri) return 0;
        std::lock_guard<std::mutex> lock(fMutex);
        for (size_t i = 0; i < fUris.size(); i++)
            if (fUris[i] == uri) return (LV2_URID)(i + 1);   // 0 means "unmapped"
        fUris.push_back(uri);
        return (LV2_URID)fUris.size();
    }

    const LV2_Feature* MapFeature() const   { return &fMapFeature; }
    const LV2_Feature* UnmapFeature() const { return &fUnmapFeature; }

private:
    static LV2_URID MapCb(LV2_URID_Map_Handle h, const char* uri) {
        return static_cast<UridMap*>(h)->Map(uri);
    }
    static const char* UnmapCb(LV2_URID_Unmap_Handle h, LV2_URID urid) {
        UridMap* self = static_cast<UridMap*>(h);
        std::lock_guard<std::mutex> lock(self->fMutex);
        if (urid == 0 || urid > self->fUris.size()) return nullptr;
        return self->fUris[urid - 1].c_str();
    }

    std::mutex               fMutex;
    std::vector<std::string> fUris;
    LV2_URID_Map             fMap{};
    LV2_URID_Unmap           fUnmap{};
    LV2_Feature              fMapFeature{};
    LV2_Feature              fUnmapFeature{};
};

// Everything we pass to lilv_plugin_instantiate. A plugin requiring anything NOT
// on this list is skipped rather than instantiated hopefully — the spec allows a
// plugin whose required feature is missing to fail hard.
//
// boundedBlockLength is on the list because Process()'s chunking makes it
// literally true: no run() ever sees more than kMaxLv2BlockFrames. Without the
// chunk loop this would be a lie we told every plugin at instantiation.
const std::vector<std::string>& SupportedFeatures() {
    static const std::vector<std::string> v = {
        LV2_URID__map,
        LV2_URID__unmap,
        LV2_OPTIONS__options,
        LV2_BUF_SIZE__boundedBlockLength,
    };
    return v;
}

struct Lv2ControlMeta {
    float mn = 0.0f, mx = 1.0f, def = 0.0f;
    bool  hasMin = false, hasMax = false;
};

// ---------------------------------------------------------------------------
// Lv2Effect
// ---------------------------------------------------------------------------

// One LV2 plugin hosted as an insert.
//
// Owns its LilvInstance(s), every scratch buffer they read and write, and the
// control-port storage the ports are connected to. All allocation happens in
// Instantiate (constructor / Prepare, off the RT thread); Process only does
// arithmetic and lilv_instance_run over what is already there.
class Lv2Effect : public IEffect {
public:
    Lv2Effect(const LilvPlugin* plugin, const Lv2PortLayout& layout,
              const std::vector<Lv2ControlMeta>& ctrl, std::string name,
              UridMap& urids, double sampleRate)
        : fPlugin(plugin), fLayout(layout), fCtrl(ctrl),
          fName(std::move(name)), fUrids(urids) {
        fSequenceType = fUrids.Map(LV2_ATOM__Sequence);

        fControlIn.resize(fLayout.controlIn.size(), 0.0f);
        for (size_t i = 0; i < fControlIn.size() && i < fCtrl.size(); i++)
            fControlIn[i] = fCtrl[i].def;
        fControlOut.resize(fLayout.controlOut.size(), 0.0f);

        for (int ch = 0; ch < 2; ch++) {
            fIn[ch].resize(kMaxLv2BlockFrames, 0.0f);
            fOut[ch].resize(kMaxLv2BlockFrames, 0.0f);
        }
        if (!fLayout.silencePorts.empty())
            fSilence.resize(kMaxLv2BlockFrames, 0.0f);
        if (!fLayout.discardPorts.empty())
            fDiscard.resize(kMaxLv2BlockFrames, 0.0f);

        Instantiate(sampleRate);
    }

    ~Lv2Effect() override { Teardown(); }

    bool Valid() const { return !fInstances.empty(); }

    void Prepare(double sampleRate) override {
        if (sampleRate <= 0.0) return;          // nothing to bind to; keep as is
        if (fInstances.empty() || sampleRate != fRate) {
            Instantiate(sampleRate);
            return;
        }
        Reset();
        LatchLatency();   // new Prepare lifetime, so re-reading is legal here
    }

    void Process(float* stereo, int frames) override {
        if (!stereo || frames <= 0 || fInstances.empty()) return;   // pass through

        int done = 0;
        while (done < frames) {
            const int n = Lv2ChunkFrames(frames - done);
            if (n <= 0) break;
            float* block = stereo + (size_t)done * 2;

            for (int i = 0; i < n; i++) {
                fIn[0][(size_t)i] = block[(size_t)i * 2];
                fIn[1][(size_t)i] = block[(size_t)i * 2 + 1];
            }

            // Re-silence the unrouted-input buffer each chunk. Every optional
            // input port shares it, and a plugin that writes through an input
            // port would otherwise turn "no sidechain connected" into whatever
            // it left behind last block.
            if (!fSilence.empty())
                std::memset(fSilence.data(), 0, (size_t)n * sizeof(float));

            for (Inst& inst : fInstances) {
                for (std::vector<uint64_t>& b : inst.atomIn)
                    ResetAtomSequence(b.data(), fSequenceType, false);
                for (std::vector<uint64_t>& b : inst.atomOut)
                    ResetAtomSequence(b.data(), fSequenceType, true);
                lilv_instance_run(inst.handle, (uint32_t)n);
            }

            for (int i = 0; i < n; i++) {
                block[(size_t)i * 2]     = fOut[0][(size_t)i];
                block[(size_t)i * 2 + 1] = fOut[1][(size_t)i];
            }
            done += n;
        }
    }

    // Deactivate/activate is LV2's documented way to return a plugin to its
    // initial state. It must NOT re-read the latency port: the engine calls
    // Reset() mid-playback on a seek, and IEffect requires LatencySamples() to
    // stay constant for the whole Prepare()/Process() lifetime — every delay
    // line sized from it (PDC, and RunInsertSlot's per-insert dry delay) has
    // already been built.
    void Reset() override {
        for (Inst& inst : fInstances) {
            lilv_instance_deactivate(inst.handle);
            lilv_instance_activate(inst.handle);
        }
    }

    // Plain float store into the buffer the control port is already connected
    // to: one writer, and LV2 control ports are re-read by the plugin on every
    // run(). With a MonoDual plugin both instances point at this same array, so
    // one write drives both channels — which is the point of sharing it.
    void SetParam(int slot, float value) override {
        if (slot < 0 || (size_t)slot >= fControlIn.size()) return;
        const Lv2ControlMeta& m = fCtrl[(size_t)slot];
        fControlIn[(size_t)slot] =
            ClampLv2Param(value, m.mn, m.mx, m.hasMin, m.hasMax);
    }

    int LatencySamples() const override { return fLatency; }
    const char* Name() const override { return fName.c_str(); }

private:
    struct Inst {
        LilvInstance* handle = nullptr;
        std::vector<std::vector<uint64_t>> atomIn;
        std::vector<std::vector<uint64_t>> atomOut;
    };

    void Teardown() {
        for (Inst& inst : fInstances) {
            if (!inst.handle) continue;
            lilv_instance_deactivate(inst.handle);
            lilv_instance_free(inst.handle);
        }
        fInstances.clear();
    }

    // Rebuild the options block for `rate`. Held as members because LV2 options
    // are borrowed by the plugin for its whole lifetime, not copied at
    // instantiation — every pointer here must outlive the instance.
    void BuildFeatures(double rate) {
        fOptSampleRate = (float)rate;
        fOptMinBlock   = 1;
        fOptMaxBlock   = kMaxLv2BlockFrames;
        fOptSeqSize    = (int32_t)kAtomBufBytes;

        const LV2_URID atomFloat = fUrids.Map(LV2_ATOM__Float);
        const LV2_URID atomInt   = fUrids.Map(LV2_ATOM__Int);

        fOptions.clear();
        fOptions.push_back({ LV2_OPTIONS_INSTANCE, 0,
                             fUrids.Map(LV2_PARAMETERS__sampleRate),
                             sizeof(float), atomFloat, &fOptSampleRate });
        fOptions.push_back({ LV2_OPTIONS_INSTANCE, 0,
                             fUrids.Map(LV2_BUF_SIZE__minBlockLength),
                             sizeof(int32_t), atomInt, &fOptMinBlock });
        fOptions.push_back({ LV2_OPTIONS_INSTANCE, 0,
                             fUrids.Map(LV2_BUF_SIZE__maxBlockLength),
                             sizeof(int32_t), atomInt, &fOptMaxBlock });
        fOptions.push_back({ LV2_OPTIONS_INSTANCE, 0,
                             fUrids.Map(LV2_BUF_SIZE__sequenceSize),
                             sizeof(int32_t), atomInt, &fOptSeqSize });
        fOptions.push_back({ LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr });

        fOptionsFeature = { LV2_OPTIONS__options, fOptions.data() };
        fBoundedFeature = { LV2_BUF_SIZE__boundedBlockLength, nullptr };

        fFeatures.clear();
        fFeatures.push_back(fUrids.MapFeature());
        fFeatures.push_back(fUrids.UnmapFeature());
        fFeatures.push_back(&fOptionsFeature);
        fFeatures.push_back(&fBoundedFeature);
        fFeatures.push_back(nullptr);            // lilv wants a NULL terminator
    }

    bool Instantiate(double rate) {
        Teardown();
        const int n = fLayout.instances;
        if (n <= 0) return false;

        BuildFeatures(rate);

        // Size every instance's buffers BEFORE instantiating any of them: the
        // connect_port calls below hand out interior pointers, and growing
        // fInstances afterwards would move the Inst objects and dangle them all.
        fInstances.resize((size_t)n);
        for (Inst& inst : fInstances) {
            inst.atomIn.assign(fLayout.atomIn.size(),
                               std::vector<uint64_t>(kAtomBufWords, 0));
            inst.atomOut.assign(fLayout.atomOut.size(),
                                std::vector<uint64_t>(kAtomBufWords, 0));
        }

        for (Inst& inst : fInstances) {
            inst.handle = lilv_plugin_instantiate(fPlugin, rate, fFeatures.data());
            if (!inst.handle) { Teardown(); return false; }
        }

        for (int k = 0; k < n; k++) ConnectPorts(k);
        for (Inst& inst : fInstances) lilv_instance_activate(inst.handle);

        fRate = rate;
        LatchLatency();
        return true;
    }

    void ConnectPorts(int k) {
        Inst& inst = fInstances[(size_t)k];
        LilvInstance* h = inst.handle;

        // Control storage is SHARED by every instance: one knob, both channels.
        // For output controls that means the two instances race to write the
        // same slot, which is harmless — we only read latency out of it, and a
        // plugin's two mono halves report the same latency.
        for (size_t i = 0; i < fLayout.controlIn.size(); i++)
            lilv_instance_connect_port(h, fLayout.controlIn[i], &fControlIn[i]);
        for (size_t i = 0; i < fLayout.controlOut.size(); i++)
            lilv_instance_connect_port(h, fLayout.controlOut[i], &fControlOut[i]);

        if (fLayout.topology == Lv2Topology::Stereo) {
            lilv_instance_connect_port(h, fLayout.audioIn[0],  fIn[0].data());
            lilv_instance_connect_port(h, fLayout.audioIn[1],  fIn[1].data());
            lilv_instance_connect_port(h, fLayout.audioOut[0], fOut[0].data());
            lilv_instance_connect_port(h, fLayout.audioOut[1], fOut[1].data());
        } else {
            // MonoDual: instance k IS channel k, so it reads and writes only
            // that channel's scratch.
            lilv_instance_connect_port(h, fLayout.audioIn[0],  fIn[k].data());
            lilv_instance_connect_port(h, fLayout.audioOut[0], fOut[k].data());
        }

        for (size_t i = 0; i < fLayout.atomIn.size(); i++)
            lilv_instance_connect_port(h, fLayout.atomIn[i], inst.atomIn[i].data());
        for (size_t i = 0; i < fLayout.atomOut.size(); i++)
            lilv_instance_connect_port(h, fLayout.atomOut[i], inst.atomOut[i].data());

        // Audio ports we are not routing: silence in, scratch out. Several ports
        // sharing one buffer is fine — nothing reads the discard buffer, and the
        // silence buffer is re-zeroed every chunk.
        for (uint32_t idx : fLayout.silencePorts)
            lilv_instance_connect_port(h, idx, fSilence.data());
        for (uint32_t idx : fLayout.discardPorts)
            lilv_instance_connect_port(h, idx, fDiscard.data());

        // Only ports of a class we have no buffer shape for, and which the
        // plugin explicitly said it can run without.
        for (uint32_t idx : fLayout.nullPorts)
            lilv_instance_connect_port(h, idx, nullptr);
    }

    // Read the latency port ONCE, after activate, and keep that number for the
    // rest of this Prepare lifetime. A plugin that moves its latency port at
    // runtime is thereby ignored on purpose: the value has already been baked
    // into the PDC solve and into RunInsertSlot's dry-delay line, and letting it
    // drift would misalign soft bypass and the wet/dry blend with no way for
    // either to notice.
    void LatchLatency() {
        fLatency = 0;
        if (fLayout.latencyOut < 0) return;
        if ((size_t)fLayout.latencyOut >= fControlOut.size()) return;

        const float v = fControlOut[(size_t)fLayout.latencyOut];
        if (!(v == v) || v <= 0.0f) return;                  // NaN or none
        if (v > (float)kMaxLatencyFrames) { fLatency = kMaxLatencyFrames; return; }
        fLatency = (int)(v + 0.5f);
    }

    // A sane ceiling on a self-reported number that sizes host-side delay lines.
    // ~2 minutes at 96 kHz: far past any real plugin, but it keeps a garbage
    // reading from asking the engine for a multi-gigabyte allocation.
    static constexpr int kMaxLatencyFrames = 1 << 24;

    const LilvPlugin*           fPlugin = nullptr;
    Lv2PortLayout               fLayout;
    std::vector<Lv2ControlMeta> fCtrl;
    std::string                 fName;
    UridMap&                    fUrids;

    std::vector<Inst>  fInstances;
    std::vector<float> fControlIn, fControlOut;
    std::vector<float> fIn[2], fOut[2];
    std::vector<float> fSilence, fDiscard;   // unrouted optional audio ports

    double fRate    = 0.0;
    int    fLatency = 0;

    LV2_URID fSequenceType = 0;

    float   fOptSampleRate = 44100.0f;
    int32_t fOptMinBlock = 1, fOptMaxBlock = kMaxLv2BlockFrames;
    int32_t fOptSeqSize  = (int32_t)kAtomBufBytes;
    std::vector<LV2_Options_Option> fOptions;
    LV2_Feature fOptionsFeature{}, fBoundedFeature{};
    std::vector<const LV2_Feature*> fFeatures;
};

} // namespace

// ---------------------------------------------------------------------------
// Lv2Host
// ---------------------------------------------------------------------------

struct Lv2Host::Impl {
    LilvWorld* world = nullptr;
    UridMap    urids;
    bool       scanned = false;

    // Parallel to `plugins`: everything Create needs that the public listing
    // does not expose.
    struct Entry {
        const LilvPlugin*           plugin = nullptr;
        Lv2PortLayout               layout;
        std::vector<Lv2ControlMeta> ctrl;
    };
    std::vector<Lv2PluginInfo> plugins;
    std::vector<Entry>         entries;
    std::vector<Lv2RejectInfo> rejected;
};

Lv2Host::Lv2Host() : fImpl(new Impl) {
    fImpl->world = lilv_world_new();
}

Lv2Host::~Lv2Host() {
    if (fImpl && fImpl->world) lilv_world_free(fImpl->world);
}

Lv2Host& Lv2Host::Instance() {
    static Lv2Host inst;
    return inst;
}

const std::vector<Lv2PluginInfo>& Lv2Host::Plugins() const { return fImpl->plugins; }
const std::vector<Lv2RejectInfo>& Lv2Host::Rejected() const { return fImpl->rejected; }

const Lv2PluginInfo* Lv2Host::Find(const std::string& uri) const {
    for (const Lv2PluginInfo& p : fImpl->plugins)
        if (p.uri == uri) return &p;
    return nullptr;
}

namespace {

// Free-function trampoline: EffectFactory's hook is a plain function pointer and
// cannot capture, so route through the singleton. Ownership transfers to the
// caller on return — MakeEffect wraps the raw pointer immediately.
IEffect* Lv2Trampoline(const EffectDesc& desc, double sampleRate) {
    return Lv2Host::Instance().Create(desc.pluginName, sampleRate).release();
}

// Read one node as a float, reporting whether it was there at all.
bool NodeAsFloat(const LilvNode* n, float* out) {
    if (!n) return false;
    if (lilv_node_is_float(n)) { *out = lilv_node_as_float(n); return true; }
    if (lilv_node_is_int(n))   { *out = (float)lilv_node_as_int(n); return true; }
    return false;
}

} // namespace

void Lv2Host::ScanAll() {
    if (fImpl->scanned) return;
    fImpl->scanned = true;

    if (!fImpl->world) return;
    lilv_world_load_all(fImpl->world);
    LilvWorld* w = fImpl->world;

    // Port classes and properties, created once and reused for every port of
    // every plugin — lilv node creation is comparatively expensive.
    LilvNode* nAudio    = lilv_new_uri(w, LV2_CORE__AudioPort);
    LilvNode* nControl  = lilv_new_uri(w, LV2_CORE__ControlPort);
    LilvNode* nInput    = lilv_new_uri(w, LV2_CORE__InputPort);
    LilvNode* nOutput   = lilv_new_uri(w, LV2_CORE__OutputPort);
    LilvNode* nAtom     = lilv_new_uri(w, LV2_ATOM__AtomPort);
    LilvNode* nOptional = lilv_new_uri(w, LV2_CORE__connectionOptional);
    LilvNode* nReports  = lilv_new_uri(w, LV2_CORE__reportsLatency);
    LilvNode* nDesig    = lilv_new_uri(w, LV2_CORE__designation);
    LilvNode* nLatency  = lilv_new_uri(w, LV2_CORE__latency);

    const LilvPlugins* all = lilv_world_get_all_plugins(w);
    LILV_FOREACH(plugins, it, all) {
        const LilvPlugin* p = lilv_plugins_get(all, it);

        const LilvNode* uriNode = lilv_plugin_get_uri(p);
        if (!uriNode) continue;
        const std::string uri = lilv_node_as_uri(uriNode);

        // Defence in depth: verify guarantees a doap:name below, but a listing
        // entry with an empty display name would be worse than one showing a URI.
        std::string name = uri;
        if (LilvNode* nameNode = lilv_plugin_get_name(p)) {
            if (const char* s = lilv_node_as_string(nameNode))
                if (*s) name = s;
            lilv_node_free(nameNode);
        }

        // A partially-parsed bundle can still surface a plugin that has a URI
        // and a plausible-looking port list, yet cannot be instantiated: one
        // installed on the dev host has a syntax error partway through its TTL,
        // so lilv publishes what it managed to read and nothing more. Listing it
        // would break the promise this whole filter exists to make — that
        // anything enumerated can actually be created — and the user would only
        // find out after putting it in a chain. lilv_plugin_verify is lilv's own
        // well-formedness check (has a name, a binary, at least one port) and
        // separates exactly that plugin from the 18 sound ones here.
        if (!lilv_plugin_verify(p)) {
            fImpl->rejected.push_back(
                { uri, name, "malformed bundle (failed lilv_plugin_verify)" });
            continue;
        }

        std::vector<std::string> required;
        if (LilvNodes* feats = lilv_plugin_get_required_features(p)) {
            LILV_FOREACH(nodes, fi, feats) {
                const LilvNode* fn = lilv_nodes_get(feats, fi);
                if (fn && lilv_node_is_uri(fn))
                    required.push_back(lilv_node_as_uri(fn));
            }
            lilv_nodes_free(feats);
        }
        if (!Lv2FeaturesSatisfied(required, SupportedFeatures())) {
            std::string missing;
            for (const std::string& r : required)
                if (!Lv2FeaturesSatisfied({ r }, SupportedFeatures()))
                    missing += (missing.empty() ? "" : ", ") + r;
            fImpl->rejected.push_back({ uri, name,
                                        "requires unsupported feature: " + missing });
            continue;
        }

        std::vector<Lv2PortSpec>    specs;
        std::vector<Lv2ControlMeta> ctrl;
        const uint32_t nPorts = lilv_plugin_get_num_ports(p);
        for (uint32_t i = 0; i < nPorts; i++) {
            const LilvPort* port = lilv_plugin_get_port_by_index(p, i);
            if (!port) continue;

            Lv2PortSpec s;
            s.index    = i;
            s.optional = lilv_port_has_property(p, port, nOptional);
            const bool isInput = lilv_port_is_a(p, port, nInput);

            if (lilv_port_is_a(p, port, nAudio)) {
                s.role = isInput ? Lv2PortRole::AudioIn : Lv2PortRole::AudioOut;
            } else if (lilv_port_is_a(p, port, nAtom)) {
                s.role = isInput ? Lv2PortRole::AtomIn : Lv2PortRole::AtomOut;
            } else if (lilv_port_is_a(p, port, nControl)) {
                s.role = isInput ? Lv2PortRole::ControlIn : Lv2PortRole::ControlOut;

                LilvNode *dn = nullptr, *mn = nullptr, *mx = nullptr;
                lilv_port_get_range(p, port, &dn, &mn, &mx);
                float f = 0.0f;
                if (NodeAsFloat(dn, &f)) s.def = f;
                if (NodeAsFloat(mn, &f)) { s.mn = f; s.hasMin = true; }
                if (NodeAsFloat(mx, &f)) { s.mx = f; s.hasMax = true; }
                lilv_node_free(dn); lilv_node_free(mn); lilv_node_free(mx);

                if (LilvNode* pn = lilv_port_get_name(p, port)) {
                    if (const char* ps = lilv_node_as_string(pn)) s.name = ps;
                    lilv_node_free(pn);
                }

                if (!isInput) {
                    // The two latency mechanisms, read SEPARATELY so the
                    // designation can take precedence over the deprecated
                    // property in BuildLv2PortLayout.
                    s.latencyProperty = lilv_port_has_property(p, port, nReports);
                    if (LilvNodes* ds = lilv_port_get_value(p, port, nDesig)) {
                        LILV_FOREACH(nodes, di, ds)
                            if (lilv_node_equals(lilv_nodes_get(ds, di), nLatency))
                                s.latencyDesignation = true;
                        lilv_nodes_free(ds);
                    }
                }
            } else {
                s.role = Lv2PortRole::Unknown;   // CV or something newer than us
            }

            if (s.role == Lv2PortRole::ControlIn)
                ctrl.push_back({ s.mn, s.mx, s.def, s.hasMin, s.hasMax });
            specs.push_back(s);
        }

        Lv2PortLayout layout = BuildLv2PortLayout(specs);
        if (layout.topology == Lv2Topology::Unsupported) {
            const std::string why =
                layout.hasUnsupportedPort
                    ? "has a required port this host cannot connect (CV or unknown)"
                    : "unsupported audio topology: " +
                          std::to_string(layout.audioIn.size()) + " in / " +
                          std::to_string(layout.audioOut.size()) + " out";
            fImpl->rejected.push_back({ uri, name, why });
            continue;
        }

        Lv2PluginInfo info;
        info.name     = name;
        info.uri      = uri;
        info.monoDual = layout.topology == Lv2Topology::MonoDual;
        for (const Lv2PortSpec& s : specs)
            if (s.role == Lv2PortRole::ControlIn)
                info.params.push_back({ s.name, s.mn, s.mx, s.def });

        fImpl->plugins.push_back(info);
        fImpl->entries.push_back({ p, layout, ctrl });
    }

    lilv_node_free(nAudio);    lilv_node_free(nControl);
    lilv_node_free(nInput);    lilv_node_free(nOutput);
    lilv_node_free(nAtom);     lilv_node_free(nOptional);
    lilv_node_free(nReports);  lilv_node_free(nDesig);
    lilv_node_free(nLatency);

    // Install the hook even when nothing was found, so an Lv2 descriptor always
    // takes the same code path (and still degrades to a null effect) whether the
    // machine has plugins or not.
    SetLv2Factory(&Lv2Trampoline);
}

std::unique_ptr<IEffect> Lv2Host::Create(const std::string& uri,
                                         double sampleRate) {
    ScanAll();
    for (size_t i = 0; i < fImpl->plugins.size(); i++) {
        if (fImpl->plugins[i].uri != uri) continue;
        const Impl::Entry& e = fImpl->entries[i];

        // Some callers (host unit tests) use MakeEffect's defaulted rate. An LV2
        // instance must bind to a concrete one, so stand in a common rate and
        // let Prepare re-instantiate when the real one arrives — the same shape
        // as PluginHost's trampoline.
        const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;

        std::unique_ptr<Lv2Effect> fx(new Lv2Effect(
            e.plugin, e.layout, e.ctrl, fImpl->plugins[i].name,
            fImpl->urids, rate));
        if (!fx->Valid()) return nullptr;        // plugin refused to instantiate
        return std::unique_ptr<IEffect>(fx.release());
    }
    return nullptr;                              // unknown or rejected URI
}

} // namespace daw
