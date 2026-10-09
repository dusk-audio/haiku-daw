#include "Lv2UiWindow.h"

#include "EffectsWindow.h"   // kMsgFxLive / kMsgFxParamCommit: the editor->model channel
#include "../plugin/Lv2Host.h"    // UiRequiresInstanceAccess: the direct-access question
#include "../plugin/Lv2UiMap.h"   // port/slot numbering + the inbound apply policy
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

#include <MessageRunner.h>
#include <View.h>

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>

namespace daw {
namespace {

// Haiku's native LV2 UI type. Anything else (X11UI, GtkUI, CocoaUI) cannot be
// embedded in a BView and is skipped. Spelled out because lv2 1.18 defines the
// other types but has no LV2_UI__BeUI macro, even though the extension has it.
const char* kNativeUiType = "http://lv2plug.in/ns/extensions/ui#BeUI";

// Self-addressed: "you are already showing this plugin, come to the front".
constexpr uint32 kMsgLv2UiRaise = 'l2ur';
// Self-addressed, from the debounce runner: a gesture has gone quiet, commit it.
constexpr uint32 kMsgLv2UiCommit = 'l2uc';

// How long a control must sit still before its live value becomes one undo
// step. Same reasoning (and the same delay) as EffectsView's wheel commit: a
// knob drag arrives as a burst of writes, and one undo entry per write would
// both flood the stack and make Ctrl-Z take a hundred presses to undo a turn.
constexpr bigtime_t kCommitDelay = 400000;   // 400 ms

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

// The one-editor-per-INSERT key: the plugin URI plus the address the editor
// writes through. Two inserts of the same plugin are different editors; the
// same insert twice is the duplicate this refuses. An editor that MOVES keeps
// this key in step (see the kMsgFxWatch handler), or reopening the insert it
// moved to would slip past the claim and open a second window on it.
std::string InsertKey(const std::string& uri, TrackId track, int fxIndex) {
    return uri + "|" + std::to_string((unsigned long long)track)
               + "|" + std::to_string(fxIndex);
}

// One editor per INSERT (see InsertKey), not per plugin.
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

// --- the live link --------------------------------------------------------

// Everything the UI's write_function needs, and nothing that outlives the
// window: the insert's address in the graph, the messenger to MainWindow, the
// port-to-slot map, and the debounce that turns a gesture into one undo step.
//
// The address is (track, fxIndex, slot), never an instance pointer. The engine
// rebuilds its chain on any structural edit and again on every play, so an
// instance cached here would be freed under a running editor; addressing the
// insert lets the engine resolve it on each write instead.
struct UiLiveLink {
    // The insert's address. Written on the window's looper (a chain edit can
    // re-point the editor), read on the editor's thread and, for a control-port
    // UI, on whichever thread the plugin writes from -- so they are atomic
    // rather than three fields that could be read half-updated.
    std::atomic<int64_t> track{kInvalidTrackId};
    std::atomic<int>     fxIndex{-1};
    BMessenger apply;      // MainWindow: owns the model, and the engine with it
    BMessenger window;     // this window: the commit timer targets it

    // False until the editor has rendered its first frame. A UI that pushes its
    // own defaults through the write function while it is being built would
    // otherwise be recorded as a live edit -- and committed as one -- the moment
    // the window opens.
    std::atomic<bool> armed{false};

    // A DIRECT_ACCESS UI: it never calls the write function, so the host has to
    // watch its port buffers instead (see the poll in Open). Both kinds are
    // linked; only the way the value is noticed differs.
    bool direct = false;
    // The value last sent for each port, so the poll reports changes and not
    // the whole board every tick. Seeded from the values the editor opened
    // with, so opening an editor is not itself an edit.
    std::vector<float> lastSeen;
    // Slot -> port index: the inverse of slotOfPort, needed to hand an inbound
    // value back to the plugin as a port_event.
    std::vector<int> portOfSlot;
    // The value last APPLIED to the plugin's GUI, per slot, so a republish of
    // unchanged values does not spam port_event.
    std::vector<float> applied;

    // Values MainWindow published for this insert, waiting for the editor
    // thread to hand them to the plugin. Guarded because they arrive on the
    // window's looper and are consumed on the editor's own thread.
    std::mutex           inboundMutex;
    std::map<int, float> inbound;

    // Port index -> the insert's parameter slot for that port, -1 for every
    // port that is not one of its control inputs. EffectDesc.params -- and so
    // kMsgFxLive's "slot" -- counts control INPUT ports in port order, the same
    // numbering Lv2Host builds the insert with.
    std::vector<int> slotOfPort;
    // The throwaway instance's control buffer, kept in step with what the UI
    // sends so a plugin reading its own port state back sees the same value.
    std::vector<float>* ctl = nullptr;

    // Guards pending and runner: taken by the UI's idle thread (a write) and by
    // the window's looper (the commit). The only lock in the write path, and it
    // is never held across a messenger call.
    std::mutex           mutex;
    std::map<int, float> pending;          // slot -> latest value, uncommitted
    BMessageRunner*      runner = nullptr; // restarted by every write

    ~UiLiveLink() { delete runner; }
};

// One parameter the user just moved in the editor: send it to the audio now,
// and arrange for the model to hear about it once the gesture stops. Both editor
// kinds funnel through here -- a control-port UI from its write_function, a
// direct-access one from the poll that watches its port buffers -- so the two
// cannot drift apart in what they commit or when.
void LinkNoteValue(UiLiveLink* link, int slot, float value) {
    if (!link || slot < 0) return;

    // Live first: the same channel the generic parameter panel uses, and what
    // makes a knob move audible as it happens rather than on a mouse-up.
    BMessage live(kMsgFxLive);
    live.AddInt64("track", (int64)link->track.load(std::memory_order_relaxed));
    live.AddInt32("fx", link->fxIndex.load(std::memory_order_relaxed));
    live.AddInt32("slot", slot);
    live.AddFloat("val", value);
    link->apply.SendMessage(&live);

    // Then the debounce. Restarting the runner on every change means only the
    // last value of a gesture reaches the model -- which is also the only one
    // worth an undo step.
    std::lock_guard<std::mutex> lock(link->mutex);
    link->pending[slot] = value;
    BMessage m(kMsgLv2UiCommit);
    // Constructed before the old one is destroyed, so a throwing allocation
    // cannot leave `runner` dangling for the destructor to delete twice.
    BMessageRunner* next = new BMessageRunner(link->window, &m, kCommitDelay, 1);
    delete link->runner;                   // restart: only the last change commits
    link->runner = next;
}

// The LV2UI_Write_Function. An LV2 UI calls this for a control change unless it
// took the direct-access route, in which case it never arrives at all.
void UiWrite(LV2UI_Controller controller, uint32_t port, uint32_t size,
             uint32_t format, const void* buffer) {
    UiLiveLink* link = (UiLiveLink*)controller;
    if (!link || !buffer) return;
    // Format 0 is LV2's plain-float control-port protocol. Anything else is a
    // protocol this host never advertised and a write we cannot interpret;
    // dropping it is the honest answer rather than guessing at the bytes.
    if (format != 0 || size != sizeof(float)) return;
    if (port >= link->slotOfPort.size()) return;
    const int slot = link->slotOfPort[port];
    if (slot < 0) return;                  // an output port, or not a control
    // Still opening: the UI is establishing its own state, not being moved by
    // the user. See `armed`.
    if (!link->armed.load(std::memory_order_acquire)) return;

    if (link->ctl && port < link->ctl->size()) (*link->ctl)[port] = *(const float*)buffer;
    if (port < link->lastSeen.size()) link->lastSeen[port] = *(const float*)buffer;
    LinkNoteValue(link, slot, *(const float*)buffer);
}

// For a UI this window is NOT linked to -- a direct-access one. Writes are
// accepted and dropped: this window's instance is not the one making sound, and
// pretending otherwise would be worse than saying so.
void UiWriteDropped(LV2UI_Controller, uint32_t, uint32_t, uint32_t,
                    const void*) {}

// Hand the plugin's GUI the values the engine published since the last tick
// (automation, or the generic parameter panel being dragged while this editor
// is open). Runs on the editor's own thread, which is where a UI expects
// port_event from -- never the window looper, where LockGL deadlocks.
//
// Applying a value also writes it into the port buffer and into the direct
// poll's "last seen", so neither direction can echo the other into a loop.
void UiApplyInbound(UiLiveLink* link, const LV2UI_Descriptor* desc,
                    LV2UI_Handle ui) {
    if (!link || !desc || !desc->port_event) return;
    std::map<int, float> in, pending;
    {
        std::lock_guard<std::mutex> lock(link->inboundMutex);
        in.swap(link->inbound);
    }
    {
        std::lock_guard<std::mutex> lock(link->mutex);
        pending = link->pending;
    }
    // The decision itself is kit-free and host-tested (Lv2UiMap.h) because the
    // window that makes it cannot be built off Haiku.
    std::vector<float> ctl;
    if (link->ctl) ctl = *link->ctl;
    const std::vector<Lv2UiPortEvent> events = Lv2UiPlanApply(
        in, link->portOfSlot, pending, link->applied, ctl, link->lastSeen);
    if (link->ctl) *link->ctl = ctl;
    for (const Lv2UiPortEvent& e : events) {
        const float v = e.value;
        desc->port_event(ui, (uint32_t)e.port, sizeof(float), 0, &v);
    }
}

// Does this UI read the DSP instance directly rather than going through the
// write_function? The answer is a fact about the installed plugin, so it is
// asked of the host layer, where it is host-testable -- the window cannot be
// built (or tested) off Haiku, and this is the decision that says whether an
// editor may be linked at all.
bool UiWantsInstanceAccess(const std::string& uri) {
    return Lv2Host::Instance().UiRequiresInstanceAccess(uri);
}

} // namespace

// ---------------------------------------------------------------------------

struct Lv2UiWindow::Impl {
    std::string uri;          // the plugin URI (what the editor is showing)
    std::string claimKey;     // uri + insert address: the one-editor-per-INSERT key
    BView*        container = nullptr;
    LilvInstance* dsp       = nullptr;
    void*         lib       = nullptr;
    const LV2UI_Descriptor* desc = nullptr;
    LV2UI_Handle  ui        = nullptr;
    const LV2UI_Idle_Interface* idle = nullptr;
    LilvUIs*      uis       = nullptr;
    // The editor's own thread: idle + inbound values + direct-access poll (see
    // Open). Never the window's looper.
    thread_id     uiThread  = -1;
    volatile bool uiRun     = false;

    // Non-null only for a control-port UI: the one this window is linked to the
    // playing insert through. Never a pointer to the engine's instance, and
    // owned here so it dies with the window whichever way Open() exits.
    std::unique_ptr<UiLiveLink> link;

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
                               const std::vector<float>& params,
                               TrackId track, int fxIndex, BMessenger apply) {
    // One editor per INSERT, not per plugin. Two tracks -- or two slots -- can
    // hold the same plugin, and each now wants its own window: an editor is
    // live and addresses its insert by index, so this is what tells them apart.
    // Two windows for the SAME insert would still be two editors claiming one
    // address, which is what this refuses.
    const std::string claimKey = InsertKey(pluginUri, track, fxIndex);

    // Already showing this insert -- or already building one? Bring the existing
    // editor forward rather than opening a rival. The claim is what makes that
    // check hold for the whole build below, which is long enough (it
    // instantiates the plugin) to matter.
    OpenClaim claim(claimKey);
    if (!claim.held)
        return nullptr;      // nothing new was opened; the caller does nothing

    Impl* d = new Impl();
    d->uri = pluginUri;
    d->claimKey = claimKey;

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
    bool directAccess = true;    // "unlinked" until the RDF says otherwise
    {
        std::lock_guard<std::mutex> worldLock(gWorldMutex);

        const LilvPlugin* plugin = FindPlugin(pluginUri);
        if (!plugin) { delete d; return nullptr; }

        const LilvUI* ui = FindNativeUi(plugin, &d->uis);
        if (!ui) { delete d; return nullptr; }

        // Which of the two modes this editor gets, decided BEFORE the window
        // exists so the title can say so from its first frame.
        directAccess = UiWantsInstanceAccess(pluginUri);

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
        // Both kinds of UI get a link; they differ in how a change is noticed
        // (write_function vs. watching the port buffers), not in what happens
        // to it afterwards.
        d->link.reset(new UiLiveLink());
        d->link->track.store((int64_t)track, std::memory_order_relaxed);
        d->link->fxIndex.store(fxIndex, std::memory_order_relaxed);
        d->link->apply   = apply;
        d->link->direct  = directAccess;
        d->link->ctl     = &d->ctl;
        // Which ports are this insert's parameters, in port order. The numbering
        // rule lives in Lv2UiMap.h (host-tested) so the slot a knob writes is
        // the slot the insert stores.
        std::vector<bool> isCtrlIn(nPorts, false);
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
                if (isIn) isCtrlIn[i] = true;
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

        d->link->slotOfPort = Lv2UiSlotsForPorts(isCtrlIn);
        // Seed the insert's STORED values so the editor opens showing what the
        // user actually set, not the plugin's factory defaults.
        for (uint32_t i = 0; i < nPorts; i++) {
            const int slot = d->link->slotOfPort[i];
            if (slot >= 0 && (size_t)slot < params.size())
                d->ctl[i] = params[(size_t)slot];
        }

        // What the editor is about to open with is not an edit: seed the poll's
        // "last seen" from the port buffers, so a direct-access editor is quiet
        // until the user actually moves something.
        d->link->lastSeen = d->ctl;

        // The inverse map, for values arriving the other way (engine -> GUI).
        // Sized by the port count, not the slot count: a slot number can exceed
        // the number of control inputs on a plugin with other port kinds.
        d->link->portOfSlot = Lv2UiPortsForSlots(d->link->slotOfPort);
        // NaN, deliberately: `applied` records what the GUI has been TOLD, and
        // it has been told nothing yet. A control-port UI learns parameter
        // values only through port_event, so seeding this with the values the
        // insert holds would make the first engine frame match every slot and
        // skip it -- the editor would sit on its factory defaults while the
        // insert played the project's values. NaN compares unequal to
        // everything, including itself, so the first frame applies all of them.
        d->link->applied = Lv2UiNothingApplied(nPorts);

        // The UI binary's location. Plain strings, so nothing past this point
        // needs the world.
        binPath    = FileUriToPath(lilv_node_as_uri(lilv_ui_get_binary_uri(ui)));
        bundlePath = FileUriToPath(lilv_node_as_uri(lilv_ui_get_bundle_uri(ui)));
        uiUri      = lilv_node_as_uri(lilv_ui_get_uri(ui));
    }

    // --- the window -------------------------------------------------------

    // Say which mode this editor is in. Both are live now, but they are not
    // the same promise: a direct-access editor's values are mirrored through
    // the host, which is worth naming rather than glossing as "live".
    std::string title = displayName.empty() ? "Plugin UI" : displayName;
    title += directAccess ? "  (live - mirrored through the host)"
                          : "  (live - drives the playing insert)";

    Lv2UiWindow* win = new Lv2UiWindow(frame, title.c_str());
    win->fImpl = d;
    RegisterOpen(claimKey, win);   // the window's ForgetOpen releases it now
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

    // A live editor commits through the window, so the debounce timer needs the
    // messenger now -- the plugin can write from its very first frame.
    if (d->link) d->link->window = BMessenger(win);

    LV2UI_Widget widget = nullptr;
    d->ui = d->desc->instantiate(d->desc, pluginUri.c_str(), bundlePath.c_str(),
                                 // A control-port UI writes through UiWrite and
                                 // drives the playing insert; a direct-access UI
                                 // never calls this at all, and gets the no-op.
                                 d->link ? (LV2UI_Write_Function)UiWrite
                                         : (LV2UI_Write_Function)UiWriteDropped,
                                 d->link ? (LV2UI_Controller)d->link.get()
                                         : nullptr,
                                 &widget, d->features.data());
    if (!d->ui) {
        winLock.Unlock(); win->PostMessage(B_QUIT_REQUESTED); return nullptr;
    }

    if (d->desc->extension_data)
        d->idle = (const LV2UI_Idle_Interface*)
                      d->desc->extension_data(LV2_UI__idleInterface);

    // Tell MainWindow to publish this insert's live values to us, and where to
    // send them. The window's own messenger, so the registration dies with the
    // window even if teardown never gets to say goodbye.
    {
        BMessage watch(kMsgFxWatch);
        watch.AddInt64("track", (int64)track);
        watch.AddInt32("fx", fxIndex);
        // The URI is how MainWindow tells, on a later chain edit, whether the
        // insert at this index is still the one this editor is showing.
        watch.AddString("uri", pluginUri.c_str());
        watch.AddMessenger("msgr", BMessenger(win));
        apply.SendMessage(&watch);
    }

    // The editor's thread -- ONE thread, doing three jobs in a fixed order:
    //
    //   apply inbound     port_event() for values the engine published
    //   idle()            render a frame of the plugin's own GUI
    //   poll own buffers  (direct-access only) notice the user's edits
    //
    // Inbound goes FIRST on purpose. A direct-access UI writes into its port
    // buffers from inside idle(); applying a (stale) engine frame after that
    // would overwrite the value the user just set, and the poll -- which runs
    // after both -- would then see no change and never send it. Engine frame
    // first, then the plugin's own frame, then look for what changed.
    //
    // It is not the window's looper: the UI renders from inside idle(), and a
    // BGLView's LockGL() deadlocks on the looper thread, which already holds the
    // window lock. port_event goes here rather than in MessageReceived for the
    // same reason -- this is the thread a plugin's GUI considers its own.
    d->uiRun = true;
    d->uiThread = spawn_thread(
        [](void* p) -> status_t {
            Impl* im = (Impl*)p;
            UiLiveLink* link = im->link.get();
            while (im->uiRun) {
                UiApplyInbound(link, im->desc, im->ui);
                if (im->idle && im->idle->idle) im->idle->idle(im->ui);

                if (!link->armed.load(std::memory_order_acquire)) {
                    // End of the editor's first frame. Whatever the plugin
                    // wrote while building itself is its OWN starting state,
                    // not an edit: take it as the baseline for the poll, then
                    // start listening. `applied` is deliberately NOT seeded --
                    // the GUI has still been told nothing, and the first engine
                    // frame is how it learns what the insert holds.
                    link->lastSeen = im->ctl;
                    link->armed.store(true, std::memory_order_release);
                    snooze(16000);
                    continue;
                }

                // A direct-access UI writes into ITS instance's port buffers and
                // never calls the write function, so the host watches those
                // buffers to hear it. They are ours (connected to the instance
                // we instantiated), so this reads memory we own while the
                // plugin's UI thread writes it -- a word-sized float load, the
                // same access the plugin makes itself.
                if (link->direct) {
                    for (size_t i = 0; i < link->lastSeen.size(); i++) {
                        if (link->slotOfPort[i] < 0) continue;   // not a parameter
                        const float v = im->ctl[i];
                        if (v == link->lastSeen[i]) continue;
                        link->lastSeen[i] = v;
                        LinkNoteValue(link, link->slotOfPort[i], v);
                    }
                }
                snooze(16000);   // ~60 Hz
            }
            return B_OK;
        },
        "lv2 ui loop", B_NORMAL_PRIORITY, d);
    resume_thread(d->uiThread);
    winLock.Unlock();
    return win;
}

Lv2UiWindow::~Lv2UiWindow() {
    Impl* d = fImpl;
    if (!d) return;
    // Stop the editor's thread BEFORE tearing the UI down, and let it finish its
    // current frame: killing it mid-render would leave the GL context locked,
    // and it is also the last thing that can read this window's port buffers or
    // post a live value.
    if (d->uiThread >= 0) {
        d->uiRun = false;
        status_t st = 0;
        wait_for_thread(d->uiThread, &st);
    }
    // Stop the engine publishing to it before its handles go away (the fifth
    // teardown rule). The registration carries the window's own messenger, so
    // this is belt and braces for an editor closed by a path that never got
    // here -- but that is exactly the case that leaves the engine publishing
    // into nothing.
    if (d->link) {
        BMessage watch(kMsgFxWatch);
        watch.AddInt64("track", (int64)d->link->track.load(std::memory_order_relaxed));
        watch.AddInt32("fx", d->link->fxIndex.load(std::memory_order_relaxed));
        watch.AddString("uri", d->uri.c_str());   // which registration this is
        d->link->apply.SendMessage(&watch);       // no messenger = stop watching
    }
    // Fifth teardown rule: tell the model about anything the editor wrote and
    // did not get to commit, BEFORE the plugin's handles go away. A knob turned
    // and the window closed inside the debounce window would otherwise be live
    // in the audio and absent from the model -- which the next engine rebuild
    // (any structural edit, and every play) would resolve in favour of the
    // stale value, undoing the gesture the user just heard.
    CommitPending();
    // Detach the container -- and with it the plugin's whole view subtree --
    // BEFORE calling the plugin's cleanup.
    //
    // cleanup() destroys the views the plugin created, and `BView::~BView()`
    // opens with an unconditional
    //     if (fOwner != NULL)
    //         debugger("Trying to delete a view that belongs to a window. "
    //                  "Call RemoveSelf first.");
    // so a plugin deleting its own still-attached view aborts the team. That is
    // exactly what killed the app whenever an LV2 editor was closed: the crash
    // was inside the plugin, but the cause was ours -- we invited it to delete
    // attached views. Removing the container clears fOwner across the whole
    // subtree, because the BeAPI propagates the owner down to every descendant,
    // so the plugin's own delete then takes the ordinary path.
    //
    // We are on the looper inside BLooper::Quit(), which asserts the window
    // lock, so this is already the locked context RemoveChild requires.
    if (d->container) RemoveChild(d->container);

    if (d->desc && d->ui && d->desc->cleanup) d->desc->cleanup(d->ui);

    // Whatever the plugin did NOT delete is still parented to the container: a
    // view it destroyed unlinked itself, since ~BView calls RemoveSelf. So
    // deleting the container takes the leftovers with it, HERE, while the
    // plugin's code is still loaded -- otherwise ~BWindow would delete them
    // after this destructor body returns, through a vtable we no longer own.
    delete d->container;
    d->container = nullptr;

    // Deliberately NOT dlclose()d.
    //
    // The plugin's UI code owns objects whose lifetime we do not fully control
    // -- views, GL contexts, DPF/pugl statics -- and unloading the library while
    // any of them survive turns an ordinary destructor into a jump into
    // unmapped memory. That is what crashed the app here: the editor closed,
    // the library went away, and ~BWindow then deleted a plugin-owned view.
    // Hosts commonly keep plugin binaries loaded for exactly this reason; one
    // handle per plugin type is a small, bounded cost.
    ForgetOpen(d->claimKey);
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
//
// kMsgLv2UiCommit is the other self-addressed message: the debounce runner
// firing, which means the gesture is over and its value belongs in the model.
void Lv2UiWindow::MessageReceived(BMessage* msg) {
    if (msg->what == kMsgLv2UiRaise) { Activate(true); return; }
    if (msg->what == kMsgLv2UiCommit) { CommitPending(); return; }
    if (msg->what == kMsgLv2UiFlush) {
        // MainWindow is about to save and wants what this editor wrote but has
        // not committed yet -- answered in the REPLY, because a commit posted
        // from here would sit in MainWindow's queue until after the save.
        Impl* d = fImpl;
        if (!d || !d->link) return;
        std::map<int, float> pending;
        {
            std::lock_guard<std::mutex> lock(d->link->mutex);
            pending.swap(d->link->pending);
            delete d->link->runner;      // this gesture is being committed now
            d->link->runner = nullptr;
        }
        BMessage reply;
        for (const auto& e : pending) {
            reply.AddInt32("slot", e.first);
            reply.AddFloat("val", e.second);
        }
        msg->SendReply(&reply);
        return;
    }
    if (msg->what == kMsgFxWatch) {
        // MainWindow telling this editor that its insert MOVED within the
        // chain (a reorder). Without this the editor would keep writing to the
        // index it used to have -- a different effect than the one it shows.
        Impl* d = fImpl;
        if (d && d->link) {
            int64 tid = 0; int32 fx = -1;
            msg->FindInt64("track", &tid);
            msg->FindInt32("fx", &fx);
            if (fx >= 0) {
                d->link->track.store((int64_t)tid, std::memory_order_relaxed);
                d->link->fxIndex.store(fx, std::memory_order_relaxed);

                // Move the claim with it. The claim IS this editor's address,
                // so leaving it behind means opening the insert it just moved
                // to is judged "free" and a second window opens on the same
                // insert -- two editors, one address, and closing either drops
                // the other's registration (which is keyed the same way).
                const std::string next = InsertKey(d->uri, (TrackId)tid, fx);
                if (next != d->claimKey) {
                    std::lock_guard<std::mutex> lock(gOpenMutex);
                    for (auto& e : gOpen)
                        if (e.first == d->claimKey) e.first = next;
                    d->claimKey = next;
                }
            }
        }
        return;
    }
    if (msg->what == kMsgFxParams) {
        // Values the engine published for this insert (int32 "gen", then one
        // float "v" per parameter in slot order). Queued for the editor's own
        // thread: port_event belongs there, not on this looper.
        Impl* d = fImpl;
        if (!d || !d->link) return;
        // Absolute values in slot order, so a dropped frame costs nothing: the
        // next one carries the whole board.
        std::lock_guard<std::mutex> lock(d->link->inboundMutex);
        for (int32 i = 0; ; i++) {
            float v = 0.0f;
            if (msg->FindFloat("v", i, &v) != B_OK) break;
            d->link->inbound[i] = v;
        }
        return;
    }
    BWindow::MessageReceived(msg);
}

// Turn the values the UI has written since the last commit into one undoable
// model edit, through MainWindow -- this window never touches the model.
void Lv2UiWindow::CommitPending() {
    Impl* d = fImpl;
    if (!d || !d->link) return;
    std::map<int, float> pending;
    {
        // Copy and clear under the lock: the UI's idle thread may be writing
        // the next gesture's values into the same map right now, and those
        // belong to the NEXT commit, not this one.
        std::lock_guard<std::mutex> lock(d->link->mutex);
        pending.swap(d->link->pending);
    }
    if (pending.empty()) return;

    BMessage m(kMsgFxParamCommit);
    m.AddInt64("track", (int64)d->link->track.load(std::memory_order_relaxed));
    m.AddInt32("fx", d->link->fxIndex.load(std::memory_order_relaxed));
    // Which plugin these values belong to. The address alone is not enough:
    // MainWindow applies this asynchronously, and by then the insert at that
    // index may be a different one -- the project may even have been replaced
    // (see MainWindow's handler). The URI is what makes a late commit harmless.
    m.AddString("uri", d->uri.c_str());
    for (const auto& e : pending) {
        m.AddInt32("slot", e.first);
        m.AddFloat("val", e.second);
    }
    d->link->apply.SendMessage(&m);
}

} // namespace daw
