#include "Lv2Host.h"

#include "Lv2PortMap.h"
#include "Lv2PresetStore.h"
#include "../dsp/EffectFactory.h"
#include "../model/Effect.h"

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/options/options.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <cstdlib>   // setenv, for the Haiku bundle search path
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

    // The bare interfaces, for lilv's state API — which takes an LV2_URID_Map*
    // rather than a feature, and must be handed the SAME table the instance was
    // given or a state's URIDs would be unmapped to whatever this table happens
    // to hold at those numbers.
    LV2_URID_Map*   MapPtr()   { return &fMap; }
    LV2_URID_Unmap* UnmapPtr() { return &fUnmap; }

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
    bool  isInteger = false;
    bool  isToggled = false;
    std::vector<float> scalePoints;
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
              UridMap& urids, LilvWorld* world, std::recursive_mutex& worldLock,
              std::string uri, const std::string& state, double sampleRate)
        : fPlugin(plugin), fLayout(layout), fCtrl(ctrl),
          fName(std::move(name)), fUrids(urids), fWorld(world),
          fWorldLock(worldLock), fUri(std::move(uri)) {
        fSequenceType = fUrids.Map(LV2_ATOM__Sequence);

        // fCtrl is built alongside fLayout.controlIn during the scan, so the two
        // should already agree. Make that an INVARIANT rather than an assumption:
        // SetParam indexes fCtrl after bounds-checking against fControlIn, and a
        // silent divergence between them would be an out-of-bounds read on a path
        // driven by automation.
        fControlIn.resize(fLayout.controlIn.size(), 0.0f);
        fCtrl.resize(fControlIn.size());
        for (size_t i = 0; i < fControlIn.size(); i++)
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
        // The insert's stored state, restored into the instance the caller is
        // about to run. It goes on AFTER instantiation and activation and
        // before anyone else can see the object: LV2's instantiation threading
        // class forbids restoring on an instance something else is running, and
        // nothing here has been handed out yet.
        if (!state.empty()) LoadState(state);
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
        // Enforced HERE, not only in the editor: automation lanes and project
        // files reach this without passing through any slider.
        fControlIn[(size_t)slot] =
            ClampLv2Param(value, m.mn, m.mx, m.hasMin, m.hasMax, m.isInteger,
                          m.isToggled, &m.scalePoints);
    }

    // The control buffer IS the plugin's parameter state between blocks: this
    // is what SetParam writes and what the plugin re-reads on every run(). The
    // engine copies it (on this thread, inside the audio block) so an open
    // native editor can be shown what automation did; nothing else can see it,
    // since automation never passes through the model.
    int ControlValues(float* out, int maxSlots) const override {
        const int n = (int)fControlIn.size() < maxSlots ? (int)fControlIn.size()
                                                        : maxSlots;
        for (int i = 0; i < n; i++) out[i] = fControlIn[(size_t)i];
        return n;
    }

    int LatencySamples() const override { return fLatency; }
    const char* Name() const override { return fName.c_str(); }

    // The plugin's own state, through its state:interface, as a lilv document.
    //
    // Port values are deliberately NOT captured (get_value == NULL): the
    // control ports are the model's `params`, and a second copy of them inside
    // the blob would be a second truth — one that automation (which writes the
    // instance, never the model) would quietly diverge from. What this saves is
    // exactly what the model cannot know.
    //
    // The feature list carries our URID map, because that is how a plugin
    // turns the property keys it stores into the URIDs the store callback
    // takes; without it a plugin has no way to name anything and fails.
    bool SaveState(std::string* out) const override {
        if (!out || fInstances.empty() || !fWorld || !fInstances[0].handle)
            return false;
        std::lock_guard<std::recursive_mutex> lock(fWorldLock);
        const LV2_Feature* features[] = {
            fUrids.MapFeature(), fUrids.UnmapFeature(), nullptr
        };
        LilvState* st = lilv_state_new_from_instance(
            fPlugin, fInstances[0].handle, fUrids.MapPtr(),
            nullptr, nullptr, nullptr, nullptr,   // no file directories
            nullptr, nullptr, 0, features);
        if (!st) return false;
        const std::string uri = fUri + "#state";
        char* str = lilv_state_to_string(fWorld, fUrids.MapPtr(),
                                         fUrids.UnmapPtr(), st, uri.c_str(),
                                         nullptr);
        lilv_state_free(st);
        if (!str) return false;
        out->assign(str);
        lilv_free(str);
        return true;
    }

    bool LoadState(const std::string& state) override {
        if (state.empty() || fInstances.empty() || !fWorld) return false;
        std::lock_guard<std::recursive_mutex> lock(fWorldLock);
        LilvState* st = lilv_state_new_from_string(fWorld, fUrids.MapPtr(),
                                                   state.c_str());
        if (!st) return false;
        const LV2_Feature* features[] = {
            fUrids.MapFeature(), fUrids.UnmapFeature(), nullptr
        };
        // One restore per instance: a MonoDual plugin is two instances, and
        // both channels must come up on the same patch.
        for (Inst& inst : fInstances)
            if (inst.handle)
                lilv_state_restore(st, inst.handle, nullptr, nullptr, 0,
                                   features);
        lilv_state_free(st);
        return true;
    }

private:
    struct Inst {
        LilvInstance* handle = nullptr;
        std::vector<std::vector<uint64_t>> atomIn;
        std::vector<std::vector<uint64_t>> atomOut;
    };

    // deactivate() is only legal on an instance that was activated. Instantiate
    // activates nothing until every instance exists, so a MonoDual plugin whose
    // SECOND instance fails to instantiate leaves a live-but-unactivated first
    // one to clean up — deactivating that is out of contract.
    void Teardown() {
        for (Inst& inst : fInstances) {
            if (!inst.handle) continue;
            if (fActivated) lilv_instance_deactivate(inst.handle);
            lilv_instance_free(inst.handle);
        }
        fInstances.clear();
        fActivated = false;
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

        // Clear the OUTPUT control values before the new instances are wired to
        // them. Input controls deliberately survive — they hold the user's
        // parameter values and re-instantiation (a sample-rate change) must not
        // silently reset every knob — but outputs are the plugin's to publish.
        // Carrying them over means a plugin that does not write its latency port
        // at activate would leave us reporting the latency it had at the OLD
        // sample rate, a number that was never true at the new one. Zero is both
        // honest and what a plugin with no latency port already yields.
        std::fill(fControlOut.begin(), fControlOut.end(), 0.0f);

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
        fActivated = true;

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
    // What serializing this instance's state takes: lilv parses and writes RDF
    // through a world, and the state document is addressed by a URI.
    LilvWorld*                  fWorld = nullptr;
    // Serializes lilv use across loopers: the running instance's state is saved
    // from the main window while the effects window may be listing presets, and
    // lilv's world is not thread-safe (see Impl::worldMutex).
    std::recursive_mutex&       fWorldLock;
    std::string                 fUri;

    std::vector<Inst>  fInstances;
    std::vector<float> fControlIn, fControlOut;
    std::vector<float> fIn[2], fOut[2];
    std::vector<float> fSilence, fDiscard;   // unrouted optional audio ports

    double fRate      = 0.0;
    int    fLatency   = 0;
    bool   fActivated = false;   // guards deactivate(); see Teardown

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

    // lilv's world is not thread-safe, and since the insert panel grew a preset
    // menu this host is reached from TWO loopers: the effects window asks for
    // presets while the main window may be instantiating a plugin or capturing
    // state. Recursive because the public entry points call each other
    // (Presets -> PresetParams) and a plain mutex would deadlock there. Never
    // taken on the audio thread -- every caller is off-RT by construction.
    std::recursive_mutex worldMutex;

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
    std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
    if (fImpl->scanned) return;
    fImpl->scanned = true;

    if (!fImpl->world) return;

    // lilv's built-in default search path is POSIX-shaped (~/.lv2,
    // /usr/local/lib/lv2, /usr/lib/lv2) and names no directory that exists on
    // Haiku. A stock Haiku install therefore finds ZERO plugins: lilv reports no
    // plugins at all rather than an error, so the feature would be silently dead
    // on the platform this DAW actually targets, with nothing in any log to say
    // why. Verified on the VM — /boot/system/lib/lv2 ships eg-amp and friends,
    // and lilv walked straight past them.
    //
    // overwrite = 0, so an LV2_PATH the user set explicitly always wins. User
    // directories come first so a locally built bundle shadows the system copy
    // of the same plugin, which is the order PluginHost scans in too.
#ifdef __HAIKU__
    setenv("LV2_PATH",
           "/boot/home/config/non-packaged/lib/lv2:"
           "/boot/home/config/lib/lv2:"
           "/boot/system/non-packaged/lib/lv2:"
           "/boot/system/lib/lv2",
           0);
#endif

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
    LilvNode* nInteger  = lilv_new_uri(w, LV2_CORE__integer);
    LilvNode* nToggled  = lilv_new_uri(w, LV2_CORE__toggled);
    LilvNode* nEnum     = lilv_new_uri(w, LV2_CORE__enumeration);

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
                // The symbol too: a state document names control ports by it,
                // and it is the only name that is stable across a re-labelled
                // plugin (a preset saved here must survive the plugin being
                // retitled in its next release).
                if (const LilvNode* sym = lilv_port_get_symbol(p, port))
                    if (const char* ss = lilv_node_as_string(sym)) s.symbol = ss;

                // Three distinct domains, kept distinct. isInteger stays true
                // for all of them ("not continuous"), while isToggled and the
                // scale points say what is actually accepted.
                s.isToggled = lilv_port_has_property(p, port, nToggled);
                const bool isEnum = lilv_port_has_property(p, port, nEnum);
                s.isInteger = s.isToggled || isEnum
                           || lilv_port_has_property(p, port, nInteger);

                // An enumeration's legal values are its scale points, and they
                // need not be contiguous -- without them "an integer in range"
                // would accept values the plugin never defined.
                if (isEnum) {
                    if (LilvScalePoints* sp = lilv_port_get_scale_points(p, port)) {
                        LILV_FOREACH(scale_points, si, sp) {
                            const LilvScalePoint* pt = lilv_scale_points_get(sp, si);
                            float f = 0.0f;
                            if (pt && NodeAsFloat(lilv_scale_point_get_value(pt), &f))
                                s.scalePoints.push_back(f);
                        }
                        lilv_scale_points_free(sp);
                    }
                    std::sort(s.scalePoints.begin(), s.scalePoints.end());
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
                ctrl.push_back({ s.mn, s.mx, s.def, s.hasMin, s.hasMax,
                                 s.isInteger, s.isToggled, s.scalePoints });
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
            if (s.role == Lv2PortRole::ControlIn) {
                info.params.push_back({ s.name, s.mn, s.mx, s.def, s.isInteger,
                                        s.isToggled, s.scalePoints });
                info.paramSymbols.push_back(s.symbol);
            }

        fImpl->plugins.push_back(info);
        fImpl->entries.push_back({ p, layout, ctrl });
    }

    lilv_node_free(nAudio);    lilv_node_free(nControl);
    lilv_node_free(nInput);    lilv_node_free(nOutput);
    lilv_node_free(nAtom);     lilv_node_free(nOptional);
    lilv_node_free(nReports);  lilv_node_free(nDesig);
    lilv_node_free(nLatency);   lilv_node_free(nInteger);
    lilv_node_free(nToggled);   lilv_node_free(nEnum);

    // Install the hook even when nothing was found, so an Lv2 descriptor always
    // takes the same code path (and still degrades to a null effect) whether the
    // machine has plugins or not.
    SetLv2Factory(&Lv2Trampoline);
}

bool Lv2Host::UiRequiresInstanceAccess(const std::string& uri) {
    std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
    ScanAll();   // idempotent: the world has to exist before it can be asked
    LilvWorld* w = fImpl->world;
    if (!w) return true;
    LilvNode* uriNode = lilv_new_uri(w, uri.c_str());
    if (!uriNode) return true;
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w),
                                                  uriNode);
    lilv_node_free(uriNode);
    if (!p) return true;

    // The BeUI the editor window would actually embed -- not merely the first
    // UI listed. A bundle may carry several (an X11UI and a BeUI, or a GtkUI
    // for another platform), they need not agree about instance-access, and
    // answering from the wrong one would call a direct-access editor
    // control-port: it would get no poll, its controls would drive nothing, and
    // its title would say the opposite. Same test Lv2UiWindow::FindNativeUi
    // uses to pick the UI it embeds.
    LilvUIs* uis = lilv_plugin_get_uis(p);
    if (!uis) return true;
    LilvNode* beui = lilv_new_uri(w, "http://lv2plug.in/ns/extensions/ui#BeUI");
    const LilvUI* ui = nullptr;
    if (beui) {
        LILV_FOREACH(uis, i, uis) {
            const LilvUI* candidate = lilv_uis_get(uis, i);
            if (candidate && lilv_ui_is_a(candidate, beui)) { ui = candidate; break; }
        }
    }
    lilv_node_free(beui);
    if (!ui) { lilv_uis_free(uis); return true; }

    LilvNode* pred = lilv_new_uri(w, LV2_CORE__requiredFeature);
    LilvNode* want = lilv_new_uri(w, LV2_INSTANCE_ACCESS_URI);
    bool needs = true;                 // conservative until proven otherwise
    if (pred && want) {
        needs = false;
        if (LilvNodes* found = lilv_world_find_nodes(w, lilv_ui_get_uri(ui),
                                                     pred, nullptr)) {
            LILV_FOREACH(nodes, i, found)
                if (lilv_node_equals(lilv_nodes_get(found, i), want)) {
                    needs = true;
                    break;
                }
            lilv_nodes_free(found);
        }
    }
    lilv_node_free(pred);
    lilv_node_free(want);
    lilv_uis_free(uis);
    return needs;
}

std::unique_ptr<IEffect> Lv2Host::Create(const std::string& uri,
                                         double sampleRate,
                                         const std::string& state) {
    ScanAll();
    for (size_t i = 0; i < fImpl->plugins.size(); i++) {
        if (fImpl->plugins[i].uri != uri) continue;
        const Impl::Entry& e = fImpl->entries[i];

        // Some callers (host unit tests) use MakeEffect's defaulted rate. An LV2
        // instance must bind to a concrete one, so stand in a common rate and
        // let Prepare re-instantiate when the real one arrives — the same shape
        // as PluginHost's trampoline.
        const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;

        std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
        std::unique_ptr<Lv2Effect> fx(new Lv2Effect(
            e.plugin, e.layout, e.ctrl, fImpl->plugins[i].name,
            fImpl->urids, fImpl->world, fImpl->worldMutex,
            fImpl->plugins[i].uri, state, rate));
        if (!fx->Valid()) return nullptr;        // plugin refused to instantiate
        return std::unique_ptr<IEffect>(fx.release());
    }
    return nullptr;                              // unknown or rejected URI
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

namespace {

// The URID of an atom type we can read out of a preset's port values. Anything
// else (a string, a path, an object) is not a knob and is skipped.
bool PortValueAsFloat(LV2_URID type, uint32_t size, const void* value,
                      LV2_URID atomFloat, LV2_URID atomInt, float* out) {
    if (type == atomFloat && size == sizeof(float)) {
        *out = *(const float*)value;
        return true;
    }
    if (type == atomInt && size == sizeof(int32_t)) {
        *out = (float)*(const int32_t*)value;
        return true;
    }
    if (type == atomFloat && size == sizeof(double)) {   // unusual but legal
        *out = (float)*(const double*)value;
        return true;
    }
    return false;
}

} // namespace

std::vector<std::pair<int, float>> Lv2Host::PresetParams(const std::string& uri,
                                                         const std::string& state) {
    std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
    ScanAll();
    std::vector<std::pair<int, float>> out;
    if (state.empty() || !fImpl->world) return out;

    // Which hostable plugin, and (for the clamp) the same per-port metadata the
    // effect itself applies — from the scan's own table, so a preset's value is
    // coerced by exactly the rules SetParam uses and cannot come out different.
    const Lv2PluginInfo* info = nullptr;
    const std::vector<Lv2ControlMeta>* ctrl = nullptr;
    for (size_t i = 0; i < fImpl->plugins.size(); i++) {
        if (fImpl->plugins[i].uri != uri) continue;
        info = &fImpl->plugins[i];
        ctrl = &fImpl->entries[i].ctrl;
        break;
    }
    if (!info || !ctrl) return out;

    struct Sink {
        const std::vector<std::string>* symbols = nullptr;
        const std::vector<Lv2ControlMeta>* ctrl = nullptr;
        LV2_URID atomFloat = 0, atomInt = 0;
        std::vector<std::pair<int, float>>* out = nullptr;
    } sink;
    sink.symbols   = &info->paramSymbols;
    sink.ctrl      = ctrl;
    sink.atomFloat = fImpl->urids.Map(LV2_ATOM__Float);
    sink.atomInt   = fImpl->urids.Map(LV2_ATOM__Int);
    sink.out       = &out;

    LilvState* st = lilv_state_new_from_string(fImpl->world,
                                               fImpl->urids.MapPtr(),
                                               state.c_str());
    if (!st) return out;
    lilv_state_emit_port_values(
        st,
        [](const char* portSymbol, void* user, const void* value,
           uint32_t size, uint32_t type) {
            Sink& s = *(Sink*)user;
            if (!portSymbol || !value || !s.symbols || !s.ctrl || !s.out)
                return;
            for (size_t i = 0; i < s.symbols->size(); i++) {
                if ((*s.symbols)[i] != portSymbol) continue;
                if (i >= s.ctrl->size()) return;
                float v = 0.0f;
                if (!PortValueAsFloat(type, size, value, s.atomFloat, s.atomInt, &v))
                    return;
                // The value a preset carries was valid for the plugin that WROTE
                // it, which need not be the version installed here: coerce it
                // through the port's declared domain rather than writing
                // something the plugin does not accept.
                const Lv2ControlMeta& m = (*s.ctrl)[i];
                v = ClampLv2Param(v, m.mn, m.mx, m.hasMin, m.hasMax, m.isInteger,
                                  m.isToggled, &m.scalePoints);
                s.out->push_back({ (int)i, v });
                return;
            }
        },
        &sink);
    lilv_state_free(st);

    std::sort(out.begin(), out.end(),
              [](const std::pair<int, float>& a, const std::pair<int, float>& b) {
                  return a.first < b.first;
              });
    return out;
}

bool Lv2Host::SaveInstanceState(const std::string& uri, void* instance,
                                std::string* out) {
    std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
    ScanAll();
    if (!instance || !out || !fImpl->world) return false;
    LilvNode* uriNode = lilv_new_uri(fImpl->world, uri.c_str());
    if (!uriNode) return false;
    const LilvPlugin* p = lilv_plugins_get_by_uri(
        lilv_world_get_all_plugins(fImpl->world), uriNode);
    lilv_node_free(uriNode);
    if (!p) return false;

    const LV2_Feature* features[] = {
        fImpl->urids.MapFeature(), fImpl->urids.UnmapFeature(), nullptr
    };
    LilvState* st = lilv_state_new_from_instance(
        p, static_cast<LilvInstance*>(instance), fImpl->urids.MapPtr(),
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, features);
    if (!st) return false;
    const std::string stateUri = uri + "#state";
    char* str = lilv_state_to_string(fImpl->world, fImpl->urids.MapPtr(),
                                     fImpl->urids.UnmapPtr(), st,
                                     stateUri.c_str(), nullptr);
    lilv_state_free(st);
    if (!str) return false;
    out->assign(str);
    lilv_free(str);
    return true;
}

std::vector<Lv2PresetInfo> Lv2Host::Presets(const std::string& uri) {
    std::lock_guard<std::recursive_mutex> lock(fImpl->worldMutex);
    ScanAll();
    std::vector<Lv2PresetInfo> out;
    if (!fImpl->world) return out;

    // The plugin's own bundled presets first: they are what the plugin's author
    // shipped, and the user's own saved ones come after so a locally saved
    // "Default" never hides the factory one.
    for (size_t i = 0; i < fImpl->plugins.size(); i++) {
        if (fImpl->plugins[i].uri != uri) continue;
        const LilvPlugin* p = fImpl->entries[i].plugin;
        if (!p) break;

        LilvWorld* w = fImpl->world;
        LilvNode* presetClass =
            lilv_new_uri(w, "http://lv2plug.in/ns/ext/presets#Preset");
        if (presetClass) {
            LilvNodes* related = lilv_plugin_get_related(p, presetClass);
            if (related) {
                LILV_FOREACH(nodes, it, related) {
                    const LilvNode* node = lilv_nodes_get(related, it);
                    if (!node || !lilv_node_is_uri(node)) continue;
                    // A preset in a bundle that is only reachable through
                    // rdfs:seeAlso is not loaded yet; ask for it explicitly.
                    lilv_world_load_resource(w, node);
                    LilvState* st =
                        lilv_state_new_from_world(w, fImpl->urids.MapPtr(), node);
                    if (!st) continue;

                    Lv2PresetInfo info;
                    if (const char* label = lilv_state_get_label(st))
                        info.name = label;
                    if (info.name.empty())
                        info.name = lilv_node_as_string(node);
                    const std::string stateUri = lilv_node_as_string(node);
                    char* str = lilv_state_to_string(
                        w, fImpl->urids.MapPtr(), fImpl->urids.UnmapPtr(), st,
                        stateUri.c_str(), nullptr);
                    lilv_state_free(st);
                    if (!str) continue;
                    info.state.assign(str);
                    lilv_free(str);
                    info.params = PresetParams(uri, info.state);
                    out.push_back(std::move(info));
                }
                lilv_nodes_free(related);
            }
            lilv_node_free(presetClass);
        }
        break;
    }

    for (const PresetEntry& e : LoadPresetsFor(PresetStoreDir(), uri)) {
        Lv2PresetInfo info;
        info.name = e.name;
        info.state = e.state;
        for (size_t i = 0; i < e.params.size(); i++)
            info.params.push_back({ (int)i, e.params[i] });
        out.push_back(std::move(info));
    }
    return out;
}

bool Lv2Host::SavePreset(const std::string& uri, const std::string& name,
                         const std::string& state,
                         const std::vector<float>& params) {
    ScanAll();
    if (!Find(uri)) return false;   // unknown or unhostable plugin
    PresetEntry e;
    e.name = name;
    e.state = state;
    e.params = params;
    return SavePresetToStore(PresetStoreDir(), uri, e);
}

} // namespace daw
