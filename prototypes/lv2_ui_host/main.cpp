// lv2_ui_host — open an LV2 plugin's own GUI in a Haiku window.
//
// The DAW hosts LV2 DSP (src/plugin/Lv2Host) but draws its own generic editor;
// it has never opened a plugin's native UI. This is the smallest honest host
// that does, kept as a prototype so the UI path can be developed and watched
// without dragging the DAW's engine, model and undo stack along.
//
// What a plugin UI needs from a host, all of it required by real plugins:
//   urid:map          - map/unmap URIs.
//   ui:parent         - the native widget to embed into. On Haiku that is a
//                       BView*, which is why this cannot be kit-free.
//   options           - block length and sample rate.
//   ui:resize         - so the UI can tell us how big it wants to be. Without
//                       it the window stays whatever we guessed.
//   instance-access   - the DSP LV2_Handle.
//   data-access       - the DSP descriptor's extension_data function.
// The last two are what let a UI talk to its DSP instance directly instead of
// through control ports. DPF plugins built with DISTRHO_PLUGIN_WANT_DIRECT_ACCESS
// REFUSE TO INSTANTIATE without BOTH, which is the single most likely reason a
// plugin UI silently fails to appear.
//
// Usage:  lv2_ui_host <plugin-uri>
//         lv2_ui_host --list
//
// Haiku-only (Interface Kit) and needs lilv.

#include <lilv/lilv.h>

#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/ui/ui.h>
#include <lv2/urid/urid.h>

#include <Application.h>
#include <View.h>
#include <Window.h>

#include <dlfcn.h>
#include <cstdlib>   // setenv, for the Haiku bundle search path

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Haiku's native LV2 UI type. A UI advertising anything else (X11UI, GtkUI...)
// cannot be embedded in a BView and is skipped.
//
// Spelled out rather than taken from a macro: lv2 1.18 defines CocoaUI, GtkUI,
// X11UI and friends but has no LV2_UI__BeUI, even though the extension defines
// the type and DPF emits it.
const char* kNativeUiType = "http://lv2plug.in/ns/extensions/ui#BeUI";

constexpr int32_t kBlockLength = 1024;
constexpr double  kSampleRate  = 48000.0;

// --- urid:map over a plain string table -----------------------------------
std::vector<std::string> gUris;

LV2_URID UridMap(LV2_URID_Map_Handle, const char* uri) {
    if (!uri) return 0;
    for (size_t i = 0; i < gUris.size(); i++)
        if (gUris[i] == uri) return (LV2_URID)(i + 1);
    gUris.push_back(uri);
    return (LV2_URID)gUris.size();
}

const char* UridUnmap(LV2_URID_Unmap_Handle, LV2_URID urid) {
    if (urid == 0 || urid > gUris.size()) return nullptr;
    return gUris[urid - 1].c_str();
}

// --- the window the plugin draws into --------------------------------------
class UiWindow : public BWindow {
public:
    UiWindow(BRect frame, const char* title)
        : BWindow(frame, title, B_TITLED_WINDOW,
                  B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {
        // The plugin is given THIS view as ui:parent and adds its own view to
        // it. It must exist and be attached before the UI is instantiated.
        fContainer = new BView(Bounds(), "container", B_FOLLOW_ALL_SIDES,
                               B_WILL_DRAW);
        AddChild(fContainer);
    }

    BView* Container() const { return fContainer; }

    bool QuitRequested() override {
        be_app->PostMessage(B_QUIT_REQUESTED);
        return true;
    }

private:
    BView* fContainer = nullptr;
};

UiWindow* gWindow = nullptr;

// ui:resize — the UI telling us how big it wants to be.
int UiResize(LV2UI_Feature_Handle, int width, int height) {
    if (!gWindow) return 1;
    if (gWindow->Lock()) {
        gWindow->ResizeTo((float)width, (float)height);
        gWindow->Unlock();
    }
    return 0;
}

// The UI writing a control-port value back to the host. A real host would push
// this into its engine; printing it is what makes the harness useful for
// watching whether a UI's widgets actually reach the DSP.
void UiWrite(LV2UI_Controller, uint32_t port, uint32_t size,
             uint32_t protocol, const void* buffer) {
    if (protocol == 0 && size == sizeof(float) && buffer)
        std::printf("  ui -> port %u = %.4f\n", port, *(const float*)buffer);
    else
        std::printf("  ui -> port %u (%u bytes, protocol %u)\n",
                    port, size, protocol);
    std::fflush(stdout);
}

std::string FileUriToPath(const char* uri) {
    char* p = lilv_file_uri_parse(uri, nullptr);
    std::string out = p ? p : "";
    lilv_free(p);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <plugin-uri> | --list\n", argv[0]);
        return 1;
    }

    BApplication app("application/x-vnd.DuskAudio-Lv2UiHost");

    // lilv's built-in default search path is POSIX-shaped (~/.lv2,
    // /usr/local/lib/lv2, /usr/lib/lv2) and names NO directory that exists on
    // Haiku, so a stock install finds zero plugins and reports no error. The DAW
    // hit this too (see Lv2Host::ScanAll); overwrite=0 keeps an explicitly-set
    // LV2_PATH winning.
    setenv("LV2_PATH",
           "/boot/home/config/non-packaged/lib/lv2:"
           "/boot/home/config/lib/lv2:"
           "/boot/system/non-packaged/lib/lv2:"
           "/boot/system/lib/lv2",
           0);

    LilvWorld* world = lilv_world_new();
    lilv_world_load_all(world);
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world);

    if (std::strcmp(argv[1], "--list") == 0) {
        LilvNode* nativeType = lilv_new_uri(world, kNativeUiType);
        LILV_FOREACH(plugins, i, plugins) {
            const LilvPlugin* p = lilv_plugins_get(plugins, i);
            LilvUIs* uis = lilv_plugin_get_uis(p);
            const unsigned n = uis ? lilv_uis_size(uis) : 0;
            LilvNode* name = lilv_plugin_get_name(p);
            std::printf("%-40s uis=%u%s\n",
                        lilv_node_as_uri(lilv_plugin_get_uri(p)), n,
                        n ? "" : "  (no UI)");
            if (name) lilv_node_free(name);
            if (uis) lilv_uis_free(uis);
        }
        lilv_node_free(nativeType);
        lilv_world_free(world);
        return 0;
    }

    const std::string wantedUri = argv[1];
    LilvNode* uriNode = lilv_new_uri(world, wantedUri.c_str());
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uriNode);
    lilv_node_free(uriNode);
    if (!plugin) {
        std::printf("plugin not found: %s\n", wantedUri.c_str());
        return 1;
    }

    // --- features shared by the DSP instance and the UI --------------------
    LV2_URID_Map   map   = { nullptr, UridMap };
    LV2_URID_Unmap unmap = { nullptr, UridUnmap };
    LV2_Feature mapF   = { LV2_URID__map,   &map };
    LV2_Feature unmapF = { LV2_URID__unmap, &unmap };

    float   optRate   = (float)kSampleRate;
    int32_t optMinBlk = 1, optMaxBlk = kBlockLength, optSeq = 8192;
    LV2_Options_Option opts[] = {
        { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_PARAMETERS__sampleRate),
          sizeof(float), UridMap(nullptr, LV2_ATOM__Float), &optRate },
        { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__minBlockLength),
          sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &optMinBlk },
        { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__maxBlockLength),
          sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &optMaxBlk },
        { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__sequenceSize),
          sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &optSeq },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr }
    };
    LV2_Feature optsF    = { LV2_OPTIONS__options, opts };
    LV2_Feature boundedF = { LV2_BUF_SIZE__boundedBlockLength, nullptr };

    // --- instantiate the DSP ----------------------------------------------
    // Needed for its own sake AND because instance-access/data-access hand the
    // UI a route to it.
    const LV2_Feature* dspFeatures[] = { &mapF, &unmapF, &optsF, &boundedF,
                                         nullptr };
    LilvInstance* dsp = lilv_plugin_instantiate(plugin, kSampleRate, dspFeatures);
    if (!dsp) {
        std::printf("DSP instantiation failed\n");
        return 1;
    }
    std::printf("DSP instantiated\n");

    // Every port must be connected before run(), even if we never run it: a
    // plugin may touch its buffers during activate.
    const uint32_t nPorts = lilv_plugin_get_num_ports(plugin);
    std::vector<std::vector<float>> audioBufs(nPorts);
    std::vector<float>              ctlBufs(nPorts, 0.0f);
    std::vector<std::vector<uint64_t>> atomBufs(nPorts);
    LilvNode* nAudio   = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode* nControl = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode* nInput   = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode* nAtom    = lilv_new_uri(world, LV2_ATOM__AtomPort);
    for (uint32_t i = 0; i < nPorts; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
        if (lilv_port_is_a(plugin, port, nAudio)) {
            audioBufs[i].assign(kBlockLength, 0.0f);
            lilv_instance_connect_port(dsp, i, audioBufs[i].data());
        } else if (lilv_port_is_a(plugin, port, nControl)) {
            LilvNode *d = nullptr, *mn = nullptr, *mx = nullptr;
            lilv_port_get_range(plugin, port, &d, &mn, &mx);
            if (d && lilv_node_is_float(d)) ctlBufs[i] = lilv_node_as_float(d);
            else if (d && lilv_node_is_int(d)) ctlBufs[i] = (float)lilv_node_as_int(d);
            lilv_node_free(d); lilv_node_free(mn); lilv_node_free(mx);
            lilv_instance_connect_port(dsp, i, &ctlBufs[i]);
        } else if (lilv_port_is_a(plugin, port, nAtom)) {
            atomBufs[i].assign(8192 / sizeof(uint64_t), 0);
            LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)atomBufs[i].data();
            seq->atom.type = UridMap(nullptr, LV2_ATOM__Sequence);
            seq->atom.size = lilv_port_is_a(plugin, port, nInput)
                                 ? sizeof(LV2_Atom_Sequence_Body)
                                 : 8192 - sizeof(LV2_Atom);
            lilv_instance_connect_port(dsp, i, atomBufs[i].data());
        } else {
            lilv_instance_connect_port(dsp, i, nullptr);
        }
    }
    lilv_instance_activate(dsp);

    // --- find a UI we can embed -------------------------------------------
    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    if (!uis || lilv_uis_size(uis) == 0) {
        std::printf("plugin has no UI\n");
        return 1;
    }

    LilvNode* nativeType = lilv_new_uri(world, kNativeUiType);
    const LilvUI* chosen = nullptr;
    LILV_FOREACH(uis, i, uis) {
        const LilvUI* ui = lilv_uis_get(uis, i);
        if (lilv_ui_is_a(ui, nativeType)) { chosen = ui; break; }
    }
    if (!chosen) {
        std::printf("no %s UI (this host can only embed the native type)\n",
                    kNativeUiType);
        LILV_FOREACH(uis, i, uis) {
            const LilvUI* ui = lilv_uis_get(uis, i);
            std::printf("  offered: %s\n",
                        lilv_node_as_uri(lilv_ui_get_uri(ui)));
        }
        return 1;
    }

    const std::string binPath =
        FileUriToPath(lilv_node_as_uri(lilv_ui_get_binary_uri(chosen)));
    const std::string bundlePath =
        FileUriToPath(lilv_node_as_uri(lilv_ui_get_bundle_uri(chosen)));
    const std::string uiUri = lilv_node_as_uri(lilv_ui_get_uri(chosen));
    std::printf("UI: %s\n  binary: %s\n", uiUri.c_str(), binPath.c_str());

    void* lib = dlopen(binPath.c_str(), RTLD_NOW);
    if (!lib) {
        std::printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    LV2UI_DescriptorFunction descFn =
        (LV2UI_DescriptorFunction)dlsym(lib, "lv2ui_descriptor");
    if (!descFn) {
        std::printf("no lv2ui_descriptor in %s\n", binPath.c_str());
        return 1;
    }

    const LV2UI_Descriptor* uiDesc = nullptr;
    for (uint32_t i = 0;; i++) {
        const LV2UI_Descriptor* d = descFn(i);
        if (!d) break;
        if (uiUri == d->URI) { uiDesc = d; break; }
    }
    if (!uiDesc) {
        std::printf("UI descriptor %s not found in binary\n", uiUri.c_str());
        return 1;
    }

    // --- window, then the UI inside it ------------------------------------
    gWindow = new UiWindow(BRect(80, 80, 80 + 960, 80 + 680), "LV2 UI");
    gWindow->Show();          // the container must be attached and visible
    BView* const parent = gWindow->Container();

    LV2UI_Resize resize = { nullptr, UiResize };
    LV2_Feature resizeF   = { LV2_UI__resize, &resize };
    LV2_Feature parentF   = { LV2_UI__parent, parent };
    LV2_Feature instF     = { LV2_INSTANCE_ACCESS_URI,
                              lilv_instance_get_handle(dsp) };
    LV2_Extension_Data_Feature extData =
        { lilv_instance_get_descriptor(dsp)->extension_data };
    LV2_Feature dataF     = { LV2_DATA_ACCESS_URI, &extData };

    const LV2_Feature* uiFeatures[] = { &mapF, &unmapF, &optsF, &parentF,
                                        &resizeF, &instF, &dataF, nullptr };

    LV2UI_Widget widget = nullptr;
    LV2UI_Handle uiHandle = uiDesc->instantiate(
        uiDesc, lilv_node_as_uri(lilv_plugin_get_uri(plugin)),
        bundlePath.c_str(), UiWrite, nullptr, &widget, uiFeatures);
    if (!uiHandle) {
        std::printf("UI instantiate FAILED\n");
        return 1;
    }
    std::printf("UI instantiated, widget=%p\n", widget);

    // The idle interface is how a UI gets to run its event loop.
    const LV2UI_Idle_Interface* idle = nullptr;
    if (uiDesc->extension_data)
        idle = (const LV2UI_Idle_Interface*)
                   uiDesc->extension_data(LV2_UI__idleInterface);
    std::printf("idle interface: %s\n", idle ? "yes" : "no");

    // Idle on a thread that is NOT the window's looper.
    //
    // This is not a detail. The UI renders from inside idle(), and on Haiku a
    // BGLView's LockGL() deadlocks if called from the looper thread, which
    // already holds the window lock. Driving idle from a BMessageRunner or from
    // Pulse() would hang the whole UI on its first frame.
    struct IdleArgs { const LV2UI_Idle_Interface* idle; LV2UI_Handle h; };
    static IdleArgs args = { idle, uiHandle };
    thread_id idleThread = spawn_thread(
        [](void* p) -> status_t {
            IdleArgs* a = (IdleArgs*)p;
            while (true) {
                if (a->idle && a->idle->idle) a->idle->idle(a->h);
                snooze(16000);   // ~60 Hz
            }
            return B_OK;
        },
        "lv2 ui idle", B_NORMAL_PRIORITY, &args);
    resume_thread(idleThread);

    app.Run();

    kill_thread(idleThread);
    uiDesc->cleanup(uiHandle);
    dlclose(lib);
    lilv_instance_deactivate(dsp);
    lilv_instance_free(dsp);
    lilv_node_free(nAudio); lilv_node_free(nControl);
    lilv_node_free(nInput); lilv_node_free(nAtom);
    lilv_node_free(nativeType);
    lilv_uis_free(uis);
    lilv_world_free(world);
    return 0;
}
