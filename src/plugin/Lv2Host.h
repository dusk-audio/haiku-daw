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
#include <vector>

namespace daw {

// Deliberately mirrors PluginParamInfo / PluginInfo in PluginHost.h. The two
// hosts are separate types rather than one interface because their lifetimes and
// build guards differ, but the FIELDS are kept identical on purpose: the effects
// editor reads name/mn/mx/def out of either without knowing which it has.
struct Lv2ParamInfo {
    std::string name;
    float mn = 0.0f, mx = 1.0f, def = 0.0f;
    // Whole numbers only (lv2:integer / lv2:toggled / lv2:enumeration). The
    // editor needs this to stop a continuous slider writing 0.03 into a toggle.
    // PluginParamInfo has no equivalent -- the native add-on ABI does not
    // describe it -- so a shared reader must default this to false.
    bool  isInteger = false;
};

struct Lv2PluginInfo {
    std::string name;                    // display name (URI if none declared)
    std::string uri;                     // persisted id -> EffectDesc.pluginName
    std::vector<Lv2ParamInfo> params;    // input control ports, in slot order
    bool monoDual = false;               // hosted as 2 instances, one per channel
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

    // Instantiate `uri` bound to `sampleRate`, or null if the URI is unknown,
    // unhostable, or the plugin's own instantiation failed. The caller owns the
    // result. Control ports start at their port defaults; MakeEffect applies any
    // stored EffectDesc params over the top via SetParam.
    //
    // The returned effect is already instantiated and activated at this rate;
    // Prepare() re-instantiates only if it is later given a different one.
    std::unique_ptr<IEffect> Create(const std::string& uri, double sampleRate);

private:
    Lv2Host();
    Lv2Host(const Lv2Host&) = delete;
    Lv2Host& operator=(const Lv2Host&) = delete;

    struct Impl;
    std::unique_ptr<Impl> fImpl;
};

} // namespace daw
