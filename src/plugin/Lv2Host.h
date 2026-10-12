// Lv2Host — scans installed LV2 plugins with lilv and feeds them to
// EffectFactory as IEffect inserts.
//
// The LV2 twin of PluginHost: same shape (a singleton, a scan that installs the
// factory hook, a listing the UI can enumerate, a Create by persisted id), so
// the effects UI can treat native add-ons and LV2 plugins uniformly instead of
// special-casing each. The persisted id here is the plugin URI, which is what
// EffectDesc.pluginName carries for EffectType::Lv2.
//
// Only lilv is required, no Haiku kits, so this builds and unit-tests on Linux
// too. lilv is kept out of this header (pimpl) so callers — notably the UI —
// need neither its include path nor its types.
#pragma once

#include "../dsp/IEffect.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace daw {

// Deliberately mirrors PluginParamInfo / PluginInfo in PluginHost.h. The two
// hosts are separate types rather than one interface because their lifetimes and
// build guards differ, but the FIELDS are kept identical on purpose: the effects
// editor reads name/mn/mx/def out of either without knowing which it has.
struct Lv2ParamInfo {
    std::string name;
    float mn = 0.0f, mx = 1.0f, def = 0.0f;
    // Non-continuous domains. isInteger covers all three so a caller that only
    // knows about whole numbers still behaves sanely; the other two say WHICH
    // discrete domain, which matters because they are not interchangeable:
    // a toggle has two states, and an enumeration accepts only its listed
    // values, which may be sparse. PluginParamInfo has no equivalent -- the
    // native add-on ABI does not describe any of this -- so a shared reader
    // must default them to false/empty.
    bool  isInteger = false;
    bool  isToggled = false;
    std::vector<float> scalePoints;   // lv2:enumeration values, if declared
};

struct Lv2PluginInfo {
    std::string name;                    // display name (URI if none declared)
    std::string uri;                     // persisted id -> EffectDesc.pluginName
    std::vector<Lv2ParamInfo> params;    // input control ports, in slot order
    // Parallel to `params`: each control-input port's LV2 symbol. Kept apart
    // from Lv2ParamInfo so that struct stays field-for-field the add-on's
    // PluginParamInfo (the editor swaps them without knowing which it has).
    // Needed by presets: a state document addresses control ports by SYMBOL.
    std::vector<std::string> paramSymbols;
    bool monoDual = false;               // hosted as 2 instances, one per channel
};

// One preset offered for a plugin: the label to show, the state document to
// hand an instance, and the control-port values that state sets as (slot, value)
// pairs — a preset need not mention every port, and the ones it does not
// mention are left as the insert has them, which is what a partial preset from
// another host means.
struct Lv2PresetInfo {
    std::string name;
    std::string state;
    std::vector<std::pair<int, float>> params;
};

// A plugin found on disk that we declined, and why. Kept (rather than dropped
// silently) so the reason is inspectable: "my plugin doesn't show up" is
// otherwise an unanswerable bug report, and the tests assert against it.
struct Lv2RejectInfo {
    std::string uri;
    std::string name;
    std::string reason;
};

class Lv2Host {
public:
    static Lv2Host& Instance();
    ~Lv2Host();

    // Load every bundle on the LV2 path, classify each plugin, and install the
    // EffectFactory hook. Idempotent — the first call does the work, later ones
    // return immediately — because it is both an explicit startup step and a
    // lazy guard inside Create(). NOT RT-safe (allocates, reads the filesystem,
    // parses RDF): call it at startup, never from the audio thread.
    void ScanAll();

    // Hostable plugins only. Anything rejected is in Rejected() instead, so a
    // caller enumerating this list can never offer the user a plugin that would
    // then fail to instantiate.
    const std::vector<Lv2PluginInfo>& Plugins() const;
    const std::vector<Lv2RejectInfo>& Rejected() const;

    // Metadata for a hostable plugin URI, or null. Shaped for the effects editor,
    // which needs param names/ranges to draw knobs for a chain slot.
    const Lv2PluginInfo* Find(const std::string& uri) const;

    // Does this plugin's UI read the DSP instance DIRECTLY, instead of writing
    // through the host's write function? DPF/DAF plugins built with
    // WANT_DIRECT_ACCESS say so by requiring instance-access -- which is the
    // whole difference between an editor the host can drive through control
    // ports and one it can only mirror values into.
    //
    // Lives here rather than in the editor window because it is a fact about the
    // installed plugin, not about the window, and because the window cannot be
    // built (or tested) off Haiku. Conservative on every failure path: a plugin
    // whose RDF cannot be read is reported as direct-access, since leaving an
    // editor unlinked is the safe mistake and poking a live instance is not.
    // Non-const because asking it makes sure the scan has run (idempotent).
    bool UiRequiresInstanceAccess(const std::string& uri);

    // Instantiate `uri` bound to `sampleRate`, or null if the URI is unknown,
    // unhostable, or the plugin's own instantiation failed. The caller owns the
    // result. Control ports start at their port defaults; MakeEffect applies any
    // stored EffectDesc params over the top via SetParam.
    //
    // `state` is the insert's stored plugin state (EffectDesc.state). When it is
    // non-empty it is restored into the fresh instance HERE, before the instance
    // is handed back — i.e. before anything can run it — so a project load comes
    // up on the patch it saved rather than the plugin's factory default.
    //
    // The returned effect is already instantiated and activated at this rate;
    // Prepare() re-instantiates only if it is later given a different one.
    std::unique_ptr<IEffect> Create(const std::string& uri, double sampleRate,
                                    const std::string& state = std::string());

    // Every preset available for `uri`: the ones the plugin's bundles ship
    // (lv2:Preset), then the user's own saved ones. Empty for an unknown URI, a
    // plugin with no presets, or a machine with no preset files. Non-const
    // because asking guarantees the scan has run.
    std::vector<Lv2PresetInfo> Presets(const std::string& uri);

    // Save `state` + `params` as a user preset named `name` for `uri`, in the
    // preset store (see Lv2PresetStore.h). False when the name is empty, the URI
    // is unknown, or the file cannot be written.
    bool SavePreset(const std::string& uri, const std::string& name,
                    const std::string& state,
                    const std::vector<float>& params);

    // The control-port values a state document carries, as (slot, value) pairs
    // in slot order, clamped to each port's declared domain. What a preset from
    // a bundle needs in order to move this insert's knobs; user presets written
    // by us carry their params directly and need not go through this.
    std::vector<std::pair<int, float>> PresetParams(const std::string& uri,
                                                    const std::string& state);

    // Serialize the state of a RAW lilv instance (`LilvInstance*` as void*, so
    // this header stays free of lilv) — the plugin's own editor owns one of
    // those, and where the user edits a patch (a direct-access UI writes
    // straight into it) is exactly where the state is newest. Same document
    // shape as Lv2Effect::SaveState, from the same world and URID table, so the
    // two blobs are interchangeable.
    bool SaveInstanceState(const std::string& uri, void* instance,
                           std::string* out);

private:
    Lv2Host();
    Lv2Host(const Lv2Host&) = delete;
    Lv2Host& operator=(const Lv2Host&) = delete;

    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

} // namespace daw
