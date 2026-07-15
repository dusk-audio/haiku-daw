// PluginHost — loads native effect add-ons and feeds them to EffectFactory.
//
// Scans a directory for *.so plugins (see PluginApi.h), loads each with
// load_add_on(), reads its metadata, and installs a factory hook so
// MakeEffect(EffectType::Plugin) can instantiate them. Haiku-only (uses the
// add-on / image API). The rest of the app stays kit-free and plugin-agnostic.
#pragma once

#include "../dsp/IEffect.h"

#include <memory>
#include <string>
#include <vector>

namespace daw {

struct PluginParamInfo {
    std::string name;
    float mn = 0.0f, mx = 1.0f, def = 0.0f;
};

struct PluginInfo {
    std::string name;                       // display name + persisted id
    std::string path;                       // .so file it came from
    std::vector<PluginParamInfo> params;
};

class PluginHost {
public:
    static PluginHost& Instance();

    // Load every *.so in `dir`, read metadata, and install the EffectFactory
    // hook. Safe to call with a missing dir (loads nothing). Plugins whose
    // name collides with an already-loaded one are skipped.
    void ScanDir(const std::string& dir);

    const std::vector<PluginInfo>& Plugins() const { return fPlugins; }

    // Instantiate the named plugin, prepared for `sampleRate`; null if unknown.
    // Host owns the returned effect (delete via unique_ptr).
    std::unique_ptr<IEffect> Create(const std::string& name, double sampleRate);

private:
    PluginHost() = default;

    struct Loaded;
    std::vector<PluginInfo> fPlugins;
    std::vector<Loaded>     fLoaded;
};

} // namespace daw
