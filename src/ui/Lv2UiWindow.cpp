#include "Lv2UiWindow.h"

#include "UiMetrics.h"

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

#include <View.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace daw {
namespace {

// Haiku's native LV2 UI type. Anything else (X11UI, GtkUI, CocoaUI) cannot be
// embedded in a BView and is skipped. Spelled out because lv2 1.18 defines the
// other types but has no LV2_UI__BeUI macro, even though the extension has it.
const char* kNativeUiType = "http://lv2plug.in/ns/extensions/ui#BeUI";

// Self-addressed: "you are already showing this plugin, come to the front".
constexpr uint32 kMsgLv2UiRaise = 'l2ur';

constexpr int32_t kBlockLength = 1024;
constexpr double  kSampleRate  = 48000.0;
constexpr uint32_t kAtomBufBytes = 8192;

// --- urid:map, shared by every UI window --------------------------------
std::mutex               gUridMutex;
std::vector<std::string> gUris;

LV2_URID UridMap(LV2_URID_Map_Handle, const char* uri) {
    if (!uri) return 0;
    std::lock_guard<std::mutex> lock(gUridMutex);
    for (size_t i = 0; i < gUris.size(); i++)
        if (gUris[i] == uri) return (LV2_URID)(i + 1);
    gUris.push_back(uri);
    return (LV2_URID)gUris.size();
}

const char* UridUnmap(LV2_URID_Unmap_Handle, LV2_URID urid) {
    std::lock_guard<std::mutex> lock(gUridMutex);
    if (urid == 0 || urid > gUris.size()) return nullptr;
    return gUris[urid - 1].c_str();
}

std::string FileUriToPath(const char* uri) {
    if (!uri) return std::string();
    char* p = lilv_file_uri_parse(uri, nullptr);
    std::string out = p ? p : "";
    lilv_free(p);
    return out;
}

// lilv is not thread-safe, and this world is reached from several loopers: the
// effects editor's while it draws, the inspector's when a row is clicked, and
// whichever thread opens an editor. One lock around every world access.
std::mutex gWorldMutex;

// One lilv world for every UI window.
//
// Loading the world parses every installed bundle, so doing it per window would
// make opening an editor visibly slow. The DAW's Lv2Host has its own world, but
// it deliberately keeps lilv out of its header (the effects editor must not need
// lilv's include path), and reaching in for a raw LilvInstance would leak that
// everywhere. One private world here is the smaller price.
LilvWorld* UiWorld() {
    static LilvWorld* world = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        world = lilv_world_new();
        if (world) {
#ifdef __HAIKU__
            // Same reason as Lv2Host::ScanAll: lilv's default search path names
            // no directory that exists on Haiku, and reports no error when it
            // finds nothing.
            setenv("LV2_PATH",
                   "/boot/home/config/non-packaged/lib/lv2:"
                   "/boot/home/config/lib/lv2:"
                   "/boot/system/non-packaged/lib/lv2:"
                   "/boot/system/lib/lv2",
                   0);
#endif
            lilv_world_load_all(world);
        }
    }
    return world;
}

const LilvPlugin* FindPlugin(const std::string& uri) {
    LilvWorld* w = UiWorld();
    if (!w) return nullptr;
    LilvNode* n = lilv_new_uri(w, uri.c_str());
    if (!n) return nullptr;
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w), n);
    lilv_node_free(n);
    return p;
}

// The plugin's embeddable UI, or null.
const LilvUI* FindNativeUi(const LilvPlugin* plugin, LilvUIs** owned) {
    if (!plugin) return nullptr;
    LilvWorld* w = UiWorld();
    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    if (!uis) return nullptr;
    LilvNode* type = lilv_new_uri(w, kNativeUiType);
    const LilvUI* found = nullptr;
    LILV_FOREACH(uis, i, uis) {
        const LilvUI* ui = lilv_uis_get(uis, i);
        if (lilv_ui_is_a(ui, type)) { found = ui; break; }
    }
    lilv_node_free(type);
    if (!found) { lilv_uis_free(uis); return nullptr; }
    *owned = uis;                 // caller frees once it is done with `found`
    return found;
}

// One editor per plugin.
//
// Opening a second window for the same plugin gives two editors that each
// believe they own the parameters, and each holds its own DSP instance -- so
// they disagree, silently, and neither is authoritative. Re-opening therefore
// raises the window that already exists.
// Held as BMessengers, NOT BWindow*.
//
// A raw pointer here is a use-after-free waiting to happen: the window can quit
// between the lookup and the Lock() that follows, and every failure path in
// Open() posts B_QUIT_REQUESTED and returns while the window is still going
// away. A BMessenger to a dead window simply reports !IsValid(), which is the
// difference between a stale entry and a crash.
std::mutex gOpenMutex;
std::vector<std::pair<std::string, BMessenger>> gOpen;
// Claimed, but the window does not exist yet. Building an editor takes long
// enough (it instantiates the plugin) that "is one already open?" answered only
// against gOpen would let two clicks on two loopers both get a yes.
std::vector<std::string> gOpening;

// Claim `uri` for a new editor.
//
// False means there is already one -- open, or being built right now on another
// thread -- and the existing one has been raised if it exists. A stale entry (a
// window that died without unregistering) is dropped rather than blocking the
// open forever. The claim is held until RegisterOpen or ReleaseClaim.
bool ClaimOpen(const std::string& uri) {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    for (size_t i = 0; i < gOpen.size(); i++) {
        if (gOpen[i].first != uri) continue;
        if (!gOpen[i].second.IsValid()) {     // window died without unregistering
            gOpen.erase(gOpen.begin() + (long)i);
            break;
        }
        // Asking the window to raise ITSELF keeps the work on its own looper;
        // locking someone else's window from here is what the pointer version
        // did, and it is the part that could touch freed memory.
        gOpen[i].second.SendMessage(kMsgLv2UiRaise);
        return false;
    }
    for (const std::string& u : gOpening)
        if (u == uri) return false;           // another thread is mid-open
    gOpening.push_back(uri);
    return true;
}

void ReleaseClaim(const std::string& uri) {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    for (size_t i = gOpening.size(); i > 0; --i)
        if (gOpening[i - 1] == uri) {
            gOpening.erase(gOpening.begin() + (long)(i - 1));
            return;
        }
}

// Turn the claim into a registration, without ever leaving the URI unclaimed.
void RegisterOpen(const std::string& uri, BWindow* w) {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    for (size_t i = gOpening.size(); i > 0; --i)
        if (gOpening[i - 1] == uri) gOpening.erase(gOpening.begin() + (long)(i - 1));
    gOpen.push_back({ uri, BMessenger(w) });
}

// Holds a claim for the length of an Open() attempt and drops it on every exit
// that did not produce a window. Handoff() is the success path: RegisterOpen
// has taken over.
struct OpenClaim {
    std::string uri;
    bool        held;
    explicit OpenClaim(const std::string& u) : uri(u), held(ClaimOpen(u)) {}
    ~OpenClaim() { if (held) ReleaseClaim(uri); }
    void Handoff() { held = false; }
    OpenClaim(const OpenClaim&) = delete;
    OpenClaim& operator=(const OpenClaim&) = delete;
};

// Window lock as RAII, with an explicit early release for the paths that hand
// the window back (or hand it to B_QUIT_REQUESTED). BWindow::Lock() is
// recursive for the calling thread, so plugin code that adds child views while
// this is held still works.
struct WindowLock {
    BWindow* win;
    bool     held;
    explicit WindowLock(BWindow* w) : win(w), held(w->Lock()) {}
    ~WindowLock() { Unlock(); }
    void Unlock() { if (held) { win->Unlock(); held = false; } }
    WindowLock(const WindowLock&) = delete;
    WindowLock& operator=(const WindowLock&) = delete;
};

void ForgetOpen(const std::string& uri) {
    std::lock_guard<std::mutex> lock(gOpenMutex);
    for (size_t i = gOpen.size(); i > 0; --i)
        if (gOpen[i - 1].first == uri) gOpen.erase(gOpen.begin() + (long)(i - 1));
}

} // namespace

// ---------------------------------------------------------------------------

struct Lv2UiWindow::Impl {
    std::string uri;          // registry key
    BView*        container = nullptr;
    LilvInstance* dsp       = nullptr;
    void*         lib       = nullptr;
    const LV2UI_Descriptor* desc = nullptr;
    LV2UI_Handle  ui        = nullptr;
    const LV2UI_Idle_Interface* idle = nullptr;
    LilvUIs*      uis       = nullptr;
    thread_id     idleThread = -1;
    volatile bool idleRun   = false;

    // Port buffers the DSP instance is connected to. Every port must be
    // connected before activate, even though this instance never processes.
    std::vector<std::vector<float>>    audio;
    std::vector<float>                 ctl;
    std::vector<std::vector<uint64_t>> atom;

    // Borrowed by the plugin for its whole life, so they live as long as we do.
    LV2_URID_Map   map{};
    LV2_URID_Unmap unmap{};
    LV2_Options_Option opts[5]{};
    LV2_Extension_Data_Feature extData{};
    float   optRate = (float)kSampleRate;
    int32_t optMin = 1, optMax = kBlockLength, optSeq = (int32_t)kAtomBufBytes;
    LV2_Feature fMap{}, fUnmap{}, fOpts{}, fParent{}, fInst{}, fData{};
    std::vector<const LV2_Feature*> features;
};

bool Lv2UiWindow::HasNativeUi(const std::string& pluginUri) {
    // Cached because the effects editor asks this while DRAWING, for every LV2
    // panel, on every repaint -- and answering it means walking the plugin's
    // RDF and allocating a LilvUIs each time. The answer cannot change while
    // the app runs: it is a property of what is installed on disk.
    static std::mutex cacheMutex;
    static std::vector<std::pair<std::string, bool>> cache;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (const auto& e : cache)
            if (e.first == pluginUri) return e.second;
    }

    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(gWorldMutex);
        if (const LilvPlugin* p = FindPlugin(pluginUri)) {
            LilvUIs* owned = nullptr;
            ok = FindNativeUi(p, &owned) != nullptr;
            if (owned) lilv_uis_free(owned);
        }
    }
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.push_back({ pluginUri, ok });
    return ok;
}

Lv2UiWindow::Lv2UiWindow(BRect frame, const char* title)
    : BWindow(frame, title, B_TITLED_WINDOW,
              B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS) {}

Lv2UiWindow* Lv2UiWindow::Open(BRect frame, const std::string& pluginUri,
                               const std::string& displayName,
                               const std::vector<float>& params) {
    // Already showing this plugin -- or already building one? Bring the existing
    // editor forward rather than opening a rival. The claim is what makes that
    // check hold for the whole build below, which is long enough (it
    // instantiates the plugin) to matter.
    OpenClaim claim(pluginUri);
    if (!claim.held)
        return nullptr;      // nothing new was opened; the caller does nothing

    Impl* d = new Impl();
    d->uri = pluginUri;

    // --- everything that touches lilv, BEFORE any window exists -----------
    //
    // The world is NOT thread-safe and is also read by EffectsView::Draw on
    // another looper, so it needs the lock. It is taken AND DROPPED before the
    // window is created on purpose. Once the window is shown, its looper can be
    // running ~Lv2UiWindow, which holds the WINDOW lock and then takes
    // gWorldMutex to free its lilv objects; taking the two in the opposite
    // order here -- world, then window -- is a deadlock. Keeping the two halves
    // disjoint is what makes the window lock below safe to hold.
    std::string binPath, bundlePath, uiUri;
    {
        std::lock_guard<std::mutex> worldLock(gWorldMutex);

        const LilvPlugin* plugin = FindPlugin(pluginUri);
        if (!plugin) { delete d; return nullptr; }

        const LilvUI* ui = FindNativeUi(plugin, &d->uis);
        if (!ui) { delete d; return nullptr; }

        // --- features -----------------------------------------------------
        d->map   = { nullptr, UridMap };
        d->unmap = { nullptr, UridUnmap };
        d->fMap   = { LV2_URID__map,   &d->map };
        d->fUnmap = { LV2_URID__unmap, &d->unmap };
        d->opts[0] = { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_PARAMETERS__sampleRate),
                       sizeof(float), UridMap(nullptr, LV2_ATOM__Float), &d->optRate };
        d->opts[1] = { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__minBlockLength),
                       sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &d->optMin };
        d->opts[2] = { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__maxBlockLength),
                       sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &d->optMax };
        d->opts[3] = { LV2_OPTIONS_INSTANCE, 0, UridMap(nullptr, LV2_BUF_SIZE__sequenceSize),
                       sizeof(int32_t), UridMap(nullptr, LV2_ATOM__Int), &d->optSeq };
        d->opts[4] = { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr };
        d->fOpts = { LV2_OPTIONS__options, d->opts };

        const LV2_Feature bounded = { LV2_BUF_SIZE__boundedBlockLength, nullptr };
        const LV2_Feature* dspFeatures[] = { &d->fMap, &d->fUnmap, &d->fOpts,
                                             &bounded, nullptr };

        // --- our own DSP instance, purely to satisfy instance-access -------
        d->dsp = lilv_plugin_instantiate(plugin, kSampleRate, dspFeatures);
        if (!d->dsp) { lilv_uis_free(d->uis); delete d; return nullptr; }

        LilvWorld* w = UiWorld();
        LilvNode* nAudio   = lilv_new_uri(w, LV2_CORE__AudioPort);
        LilvNode* nControl = lilv_new_uri(w, LV2_CORE__ControlPort);
        LilvNode* nInput   = lilv_new_uri(w, LV2_CORE__InputPort);
        LilvNode* nAtom    = lilv_new_uri(w, LV2_ATOM__AtomPort);

        const uint32_t nPorts = lilv_plugin_get_num_ports(plugin);
        d->audio.resize(nPorts);
        d->atom.resize(nPorts);
        d->ctl.assign(nPorts, 0.0f);
        size_t ctlInSlot = 0;
        for (uint32_t i = 0; i < nPorts; i++) {
            const LilvPort* port = lilv_plugin_get_port_by_index(plugin, i);
            if (!port) continue;
            const bool isIn = lilv_port_is_a(plugin, port, nInput);
            if (lilv_port_is_a(plugin, port, nAudio)) {
                d->audio[i].assign(kBlockLength, 0.0f);
                lilv_instance_connect_port(d->dsp, i, d->audio[i].data());
            } else if (lilv_port_is_a(plugin, port, nControl)) {
                LilvNode *dn = nullptr, *mn = nullptr, *mx = nullptr;
                lilv_port_get_range(plugin, port, &dn, &mn, &mx);
                if (dn && lilv_node_is_float(dn)) d->ctl[i] = lilv_node_as_float(dn);
                else if (dn && lilv_node_is_int(dn)) d->ctl[i] = (float)lilv_node_as_int(dn);
                lilv_node_free(dn); lilv_node_free(mn); lilv_node_free(mx);
                // Seed the insert's STORED value so the editor opens showing
                // what the user actually set, not the plugin's factory default.
                // Input control ports are in the same slot order
                // EffectDesc.params uses.
                if (isIn) {
                    if (ctlInSlot < params.size()) d->ctl[i] = params[ctlInSlot];
                    ctlInSlot++;
                }
                lilv_instance_connect_port(d->dsp, i, &d->ctl[i]);
            } else if (lilv_port_is_a(plugin, port, nAtom)) {
                d->atom[i].assign(kAtomBufBytes / sizeof(uint64_t), 0);
                LV2_Atom_Sequence* seq = (LV2_Atom_Sequence*)d->atom[i].data();
                seq->atom.type = UridMap(nullptr, LV2_ATOM__Sequence);
                seq->atom.size = isIn ? sizeof(LV2_Atom_Sequence_Body)
                                      : kAtomBufBytes - sizeof(LV2_Atom);
                lilv_instance_connect_port(d->dsp, i, d->atom[i].data());
            } else {
                lilv_instance_connect_port(d->dsp, i, nullptr);
            }
        }
        lilv_node_free(nAudio); lilv_node_free(nControl);
        lilv_node_free(nInput); lilv_node_free(nAtom);
        lilv_instance_activate(d->dsp);

        // The UI binary's location. Plain strings, so nothing past this point
        // needs the world.
        binPath    = FileUriToPath(lilv_node_as_uri(lilv_ui_get_binary_uri(ui)));
        bundlePath = FileUriToPath(lilv_node_as_uri(lilv_ui_get_bundle_uri(ui)));
        uiUri      = lilv_node_as_uri(lilv_ui_get_uri(ui));
    }

    // --- the window -------------------------------------------------------

    // Say in the title that this editor is not driving the audio, so the
    // limitation is visible at the moment it matters rather than surprising.
    std::string title = displayName.empty() ? "Plugin UI" : displayName;
    title += "  (view only - not linked to playback)";

    Lv2UiWindow* win = new Lv2UiWindow(frame, title.c_str());
    win->fImpl = d;
    RegisterOpen(pluginUri, win);   // the window's ForgetOpen releases it now
    claim.Handoff();

    d->container = new BView(win->Bounds(), "container", B_FOLLOW_ALL_SIDES,
                             B_WILL_DRAW);
    d->container->SetViewColor(ColHeader());
    win->AddChild(d->container);
    win->Show();          // the parent must be attached and visible first

    // The window is on screen now, so its looper is running and the user can
    // close it -- which runs ~Lv2UiWindow and DELETES `d` -- while this function
    // is still building the UI on top of it. Hold the window lock for the rest:
    // the looper needs that same lock to dispatch B_QUIT_REQUESTED, so teardown
    // cannot begin until this returns. The lock is recursive for this thread,
    // so the child views the plugin adds inside instantiate() still work, and
    // the idle thread spawned at the end simply waits the few microseconds
    // until the release below.
    WindowLock winLock(win);
    if (!winLock.held) {                // already quitting: nothing to build on
        win->PostMessage(B_QUIT_REQUESTED);
        return nullptr;
    }

    d->lib = dlopen(binPath.c_str(), RTLD_NOW);
    if (!d->lib) {
        winLock.Unlock(); win->PostMessage(B_QUIT_REQUESTED); return nullptr;
    }
    LV2UI_DescriptorFunction descFn =
        (LV2UI_DescriptorFunction)dlsym(d->lib, "lv2ui_descriptor");
    if (!descFn) {
        winLock.Unlock(); win->PostMessage(B_QUIT_REQUESTED); return nullptr;
    }
    for (uint32_t i = 0;; i++) {
        const LV2UI_Descriptor* c = descFn(i);
        if (!c) break;
        if (uiUri == c->URI) { d->desc = c; break; }
    }
    if (!d->desc) {
        winLock.Unlock(); win->PostMessage(B_QUIT_REQUESTED); return nullptr;
    }

    // instance-access AND data-access: DPF plugins built with
    // DISTRHO_PLUGIN_WANT_DIRECT_ACCESS refuse to instantiate without BOTH, and
    // that refusal is silent from the outside.
    d->fParent = { LV2_UI__parent, d->container };
    d->fInst   = { LV2_INSTANCE_ACCESS_URI, lilv_instance_get_handle(d->dsp) };
    d->extData = { lilv_instance_get_descriptor(d->dsp)->extension_data };
    d->fData   = { LV2_DATA_ACCESS_URI, &d->extData };
    d->features = { &d->fMap, &d->fUnmap, &d->fOpts, &d->fParent,
                    &d->fInst, &d->fData, nullptr };

    LV2UI_Widget widget = nullptr;
    d->ui = d->desc->instantiate(d->desc, pluginUri.c_str(), bundlePath.c_str(),
                                 // Writes are accepted and dropped: this
                                 // instance is not the one making sound, and
                                 // pretending otherwise would be worse.
                                 [](LV2UI_Controller, uint32_t, uint32_t,
                                    uint32_t, const void*) {},
                                 nullptr, &widget, d->features.data());
    if (!d->ui) {
        winLock.Unlock(); win->PostMessage(B_QUIT_REQUESTED); return nullptr;
    }

    if (d->desc->extension_data)
        d->idle = (const LV2UI_Idle_Interface*)
                      d->desc->extension_data(LV2_UI__idleInterface);

    // Idle on a thread that is NOT the window's looper. The UI renders from
    // inside idle(), and a BGLView's LockGL() deadlocks on the looper thread,
    // which already holds the window lock.
    if (d->idle && d->idle->idle) {
        d->idleRun = true;
        d->idleThread = spawn_thread(
            [](void* p) -> status_t {
                Impl* im = (Impl*)p;
                while (im->idleRun) {
                    if (im->idle && im->idle->idle) im->idle->idle(im->ui);
                    snooze(16000);   // ~60 Hz
                }
                return B_OK;
            },
            "lv2 ui idle", B_NORMAL_PRIORITY, d);
        resume_thread(d->idleThread);
    }
    winLock.Unlock();
    return win;
}

Lv2UiWindow::~Lv2UiWindow() {
    Impl* d = fImpl;
    if (!d) return;
    // Stop idling BEFORE tearing the UI down, and let the thread finish its
    // current frame: killing it mid-render would leave the GL context locked.
    if (d->idleThread >= 0) {
        d->idleRun = false;
        status_t st = 0;
        wait_for_thread(d->idleThread, &st);
    }
    if (d->desc && d->ui && d->desc->cleanup) d->desc->cleanup(d->ui);

    // Anything the plugin left attached is detached and destroyed HERE, while
    // its code is still loaded. ~BWindow deletes surviving children after this
    // destructor body returns, so a view the plugin forgot to remove would
    // otherwise be deleted through a vtable in an unloaded library.
    if (d->container && d->container->LockLooper()) {
        while (BView* child = d->container->ChildAt(0)) {
            d->container->RemoveChild(child);
            delete child;
        }
        d->container->UnlockLooper();
    }

    // Deliberately NOT dlclose()d.
    //
    // The plugin's UI code owns objects whose lifetime we do not fully control
    // -- views, GL contexts, DPF/pugl statics -- and unloading the library while
    // any of them survive turns an ordinary destructor into a jump into
    // unmapped memory. That is what crashed the app here: the editor closed,
    // the library went away, and ~BWindow then deleted a plugin-owned view.
    // Hosts commonly keep plugin binaries loaded for exactly this reason; one
    // handle per plugin type is a small, bounded cost.
    ForgetOpen(d->uri);
    // Freeing lilv objects is world access as much as creating them is.
    {
        std::lock_guard<std::mutex> worldLock(gWorldMutex);
        if (d->dsp) { lilv_instance_deactivate(d->dsp); lilv_instance_free(d->dsp); }
        if (d->uis) lilv_uis_free(d->uis);
    }
    delete d;
    fImpl = nullptr;
}

bool Lv2UiWindow::QuitRequested() { return true; }

// The registry asks the window to raise ITSELF, so the activation happens on
// this window's own looper instead of another thread reaching in.
void Lv2UiWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgLv2UiRaise) { Activate(true); return; }
    BWindow::MessageReceived(msg);
}

} // namespace daw
