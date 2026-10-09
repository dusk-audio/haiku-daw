// FxWatchTable — which native plugin editors are open, which insert each is
// showing, and what a chain edit means for them.
//
// Kit-free and callback-driven on purpose. In the app this bookkeeping lives in
// MainWindow, which cannot be built (let alone tested) off Haiku, and it is
// exactly where three real bugs lived: one editor's close clearing another's
// watch, an editor left driving whatever insert moved into its index after a
// reorder, and an editor whose insert was removed still being open. All of that
// is decision-making over plain data, so it lives here and is host-tested;
// MainWindow supplies the engine calls and the messengers.
#pragma once

#include "../model/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// What the table needs to know about an editor window, so the caller decides how
// to talk to it (a real BMessenger in the app, a recorder in tests).
class FxWatchEditor {
public:
    virtual ~FxWatchEditor() = default;
    virtual bool Alive() const = 0;                        // window still there
    virtual void AskToClose() = 0;                         // B_QUIT_REQUESTED
    virtual void InsertMoved(TrackId track, int fx) = 0;   // it is at `fx` now
    // One frame of the watched insert's live values, in slot order.
    virtual void SendFrame(const float* values, int count) = 0;
};

struct FxWatch {
    FxWatchEditor* editor = nullptr;          // not owned
    std::string    uri;                       // plugin it is showing
    TrackId        track = kInvalidTrackId;   // chain address
    int            fx    = -1;
    int            slot  = -1;                // engine watch slot, -1 = none
    // The generation last pushed to this editor. 0xFFFFFFFF is unreachable --
    // the engine's counter only ever takes even values -- which is what forces a
    // frame through after a re-registration or a rebind, where the values may
    // not have changed but the editor still needs them.
    uint32_t       gen = 0;
    static constexpr uint32_t kForcePush = 0xFFFFFFFFu;
};

// The chain an entry names, as the caller's model sees it: the plugin URIs in
// slot order, or nothing when that track (or the master chain) is gone.
struct FxChainView {
    bool                     exists = false;
    std::vector<std::string> uris;
};

enum class FxWatchAction {
    Keep,     // still the same insert
    Rebind,   // moved within the chain; f.newFx says where
    Close,    // gone, ambiguous, or its window died
};

struct FxWatchUpdate {
    std::size_t  index  = 0;
    FxWatchAction action = FxWatchAction::Keep;
    int          newFx  = -1;    // Rebind: its index now
};

// Re-check every entry against its chain and say what to do about each.
//
// Rules, in order:
//   - a dead window, or a chain that is gone: Close;
//   - two inserts carrying the SAME plugin cannot be told apart by URI, so an
//     editor among them would as likely end up driving the other one: Close
//     rather than guess (a stable per-insert id in the model is the real fix);
//   - same URI still at the same index: Keep (the common case);
//   - same URI somewhere else in the chain: Rebind;
//   - not in the chain at all: Close.
//
// The caller performs the actions -- engine watch, message to the editor -- and
// erases the entries that were closed, matching by index.
inline std::vector<FxWatchUpdate> FxWatchValidate(
    const std::vector<FxWatch>& entries,
    const std::vector<FxChainView>& chains)   // parallel to `entries`
{
    std::vector<FxWatchUpdate> out;
    out.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); i++) {
        const FxWatch& e = entries[i];
        FxWatchUpdate u;
        u.index = i;

        const FxChainView& chain = i < chains.size() ? chains[i] : FxChainView{};
        if (!e.editor || !e.editor->Alive() || !chain.exists) {
            u.action = FxWatchAction::Close;
            out.push_back(u);
            continue;
        }

        std::size_t copies = 0, found = 0;
        for (std::size_t k = 0; k < chain.uris.size(); k++) {
            if (chain.uris[k] != e.uri) continue;
            copies++;
            if (found == 0) found = k + 1;    // 1-based: 0 means "none found"
        }
        if (copies > 1) {
            u.action = FxWatchAction::Close;
            out.push_back(u);
            continue;
        }
        // The plugin is gone. (An index past the end is NOT enough on its own:
        // inserts above the watched one may have been removed, which shifts the
        // editor's insert down -- the URI search below is what decides.)
        if (copies == 0 || e.fx < 0) {
            u.action = FxWatchAction::Close;
            out.push_back(u);
            continue;
        }
        if ((std::size_t)e.fx < chain.uris.size()
            && chain.uris[(std::size_t)e.fx] == e.uri) {
            u.action = FxWatchAction::Keep;
            out.push_back(u);
            continue;
        }
        u.action = FxWatchAction::Rebind;
        u.newFx  = (int)(found - 1);
        out.push_back(u);
    }
    return out;
}

// Is this engine frame worth sending to the editor? `count` is what the engine
// published for that watch slot (0 or less means there is nothing to publish)
// and the generation is its change counter, which only moves when a value
// actually changed -- comparing it is what makes a still insert free.
//
// A pure function because it is the one decision on the push path that can be
// got wrong invisibly: skip too eagerly and an editor silently stops following
// automation, push regardless and every editor redraws 60 times a second.
inline bool FxWatchFrameIsNew(int count, uint32_t frameGen, uint32_t lastGen) {
    return count > 0 && frameGen != lastGen;
}

// The lowest engine watch slot no live entry is using, or -1 when they are all
// taken. Past that an editor still works and still writes -- it just stops
// following the engine, which the caller says once rather than silently.
inline int FxWatchFreeSlot(const std::vector<FxWatch>& entries, int slots) {
    for (int s = 0; s < slots; s++) {
        bool used = false;
        for (const FxWatch& e : entries)
            if (e.slot == s) { used = true; break; }
        if (!used) return s;
    }
    return -1;
}

// The entry an editor's own registration names, or -1. Registration is keyed by
// (uri, track, fx) -- the address the editor writes through -- so one editor's
// close can never drop another's watch, however many are open.
inline int FxWatchFind(const std::vector<FxWatch>& entries, const std::string& uri,
                       TrackId track, int fx) {
    for (std::size_t i = 0; i < entries.size(); i++) {
        const FxWatch& e = entries[i];
        if (e.uri == uri && e.track == track && e.fx == fx) return (int)i;
    }
    return -1;
}

} // namespace daw
