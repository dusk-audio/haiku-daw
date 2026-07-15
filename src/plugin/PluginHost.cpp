#include "PluginHost.h"

#include "PluginApi.h"
#include "../dsp/EffectFactory.h"

#include <image.h>

#include <dirent.h>
#include <string>

namespace daw {

struct PluginHost::Loaded {
    image_id             image = -1;
    std::string          name;
    daw_plugin_create_fn create = nullptr;
};

PluginHost& PluginHost::Instance() {
    static PluginHost inst;
    return inst;
}

// Free-function trampoline: EffectFactory's hook is a plain function pointer,
// so it can't capture. Route through the singleton. The engine Prepare()s the
// returned effect with the real rate, so the create-time rate is a placeholder.
static std::unique_ptr<IEffect> PluginTrampoline(const std::string& name) {
    return PluginHost::Instance().Create(name, 44100.0);
}

void PluginHost::ScanDir(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string fn = ent->d_name;
            if (fn.size() < 4 || fn.substr(fn.size() - 3) != ".so") continue;
            std::string path = dir + "/" + fn;

            image_id img = load_add_on(path.c_str());
            if (img < 0) continue;

            daw_plugin_name_fn        nameFn = nullptr;
            daw_plugin_param_count_fn cntFn  = nullptr;
            daw_plugin_param_info_fn  infoFn = nullptr;
            daw_plugin_create_fn      makeFn = nullptr;
            if (get_image_symbol(img, "daw_plugin_name", B_SYMBOL_TYPE_TEXT,
                                 (void**)&nameFn) != B_OK ||
                get_image_symbol(img, "daw_plugin_param_count", B_SYMBOL_TYPE_TEXT,
                                 (void**)&cntFn) != B_OK ||
                get_image_symbol(img, "daw_plugin_param_info", B_SYMBOL_TYPE_TEXT,
                                 (void**)&infoFn) != B_OK ||
                get_image_symbol(img, "daw_plugin_create", B_SYMBOL_TYPE_TEXT,
                                 (void**)&makeFn) != B_OK ||
                !nameFn || !cntFn || !infoFn || !makeFn) {
                unload_add_on(img);
                continue;
            }

            const char* pname = nameFn();
            std::string name = pname ? pname : "";
            if (name.empty()) { unload_add_on(img); continue; }

            bool dup = false;                    // reject a name we already have
            for (const PluginInfo& pi : fPlugins)
                if (pi.name == name) { dup = true; break; }
            if (dup) { unload_add_on(img); continue; }

            PluginInfo info;
            info.name = name;
            info.path = path;
            int n = cntFn();
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++) {
                const char* pn = nullptr;
                float mn = 0.0f, mx = 1.0f, def = 0.0f;
                infoFn(i, &pn, &mn, &mx, &def);
                info.params.push_back({pn ? pn : "", mn, mx, def});
            }
            fPlugins.push_back(info);
            fLoaded.push_back({img, name, makeFn});
        }
        closedir(d);
    }

    // Install the hook even for an empty dir so plugin descs load consistently.
    SetPluginFactory(&PluginTrampoline);
}

std::unique_ptr<IEffect> PluginHost::Create(const std::string& name,
                                            double sampleRate) {
    for (const Loaded& l : fLoaded) {
        if (l.name == name && l.create) {
            IEffect* e = l.create(sampleRate);
            return std::unique_ptr<IEffect>(e);
        }
    }
    return nullptr;
}

} // namespace daw
