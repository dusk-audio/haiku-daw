// UiSync — the kit-free rules behind the two pieces of UI bookkeeping that
// have gone wrong before: which open windows still belong to the project on
// screen, and whether an open console is showing the model's track layout.
//
// Both rules live here (and not inside MainWindow.cpp) because the host can
// test them, and because the bugs they prevent are IDENTITY bugs: a panel left
// on a stale track id writes the wrong project's data, and a rack left on a
// stale layout either drops every edit or draws a track that is gone. Neither
// shows up in a build, a screenshot or a green UI suite that did not think to
// look for it -- which is exactly how the mixer, the effects panel and the
// ad-hoc Sends/Instrument windows kept editing a freshly opened project.
#pragma once

#include "../model/Project.h"

#include <cstdint>
#include <vector>

namespace daw {

// --- the stale-window identity rule --------------------------------------

// Track ids are per-PROJECT: a loaded file restarts them at 1 (MainWindow's
// LoadFrom says so, because a render outliving its project would otherwise
// land on the new one's track 1). An id a window holds from the previous
// session therefore names a DIFFERENT track in the new one -- and the effects
// panel commits its WHOLE chain, which is a data-corrupting write saved as an
// undoable edit.
//
// So every window whose content is addressed by track id records the
// generation it opened against, the project's replacement bumps the
// generation, and Stale() is the whole test. The alternative -- a list of
// window types to close at each replacement site -- is what let the mixer, the
// effects panel and the Sends/Instrument windows keep editing the new project.
class ProjectScope {
public:
    using Generation = uint64_t;

    Generation Current() const { return fGeneration; }

    // The project was replaced (Open, New). The generation starts at 1 rather
    // than 0 so a default-initialised record (a window nobody opened) is not
    // accidentally "current".
    Generation Bump() { return ++fGeneration; }

    // Is a window opened at `seen` stale? A window opened AFTER the
    // replacement is not -- which is the reason to ask rather than to close by
    // window kind.
    bool Stale(Generation seen) const { return seen != fGeneration; }

private:
    Generation fGeneration = 1;
};

// --- the strip-payload refresh rule --------------------------------------

// The console's strip payload is a snapshot: one strip per track, in the
// model's order (BuildMixerStrips / AddMixerStripsPayload in MainWindow.cpp).
// Three edits leave it stale if nobody re-sends it:
//
//   TrackAdded    a new track has no strip at all, so it cannot be mixed;
//   TrackRemoved  a deleted track's strip looks live and drops every edit on
//                 the FindTrack guard, silently;
//   Reordered     the rack shows a different order than the arrangement.
//
// The kinds are named because they are not one bug with three faces: what the
// user sees differs, and so does what they lose.
enum class LayoutDrift {
    None,
    TrackAdded,
    TrackRemoved,
    Reordered,
};

// The model's track ids, in the model's order: the layout a strip payload
// describes.
inline std::vector<uint64_t> TrackLayoutOf(const Project& p) {
    std::vector<uint64_t> ids;
    ids.reserve(p.Tracks().size());
    for (const Track& t : p.Tracks()) ids.push_back((uint64_t)t.id);
    return ids;
}

// What drifted between the layout a console is showing and the model's. Ids
// are compared as a SEQUENCE (order is part of the layout: a reorder is a
// stale console even though every id is present).
inline LayoutDrift TrackLayoutDrift(const std::vector<uint64_t>& shown,
                                    const std::vector<uint64_t>& model) {
    if (shown == model) return LayoutDrift::None;
    auto holds = [](const std::vector<uint64_t>& v, uint64_t id) {
        for (uint64_t x : v) if (x == id) return true;
        return false;
    };
    // A strip whose track is gone is the one that lies: it draws state the
    // model no longer has, and every edit it posts is dropped. Report that
    // first -- with both a removal and an addition, it is the removal the user
    // cannot see.
    for (uint64_t id : shown)
        if (!holds(model, id)) return LayoutDrift::TrackRemoved;
    for (uint64_t id : model)
        if (!holds(shown, id)) return LayoutDrift::TrackAdded;
    return LayoutDrift::Reordered;   // same tracks, different order
}

// The call-site spelling of the rule above.
inline bool TrackLayoutMatches(const std::vector<uint64_t>& shown,
                               const std::vector<uint64_t>& model) {
    return TrackLayoutDrift(shown, model) == LayoutDrift::None;
}

} // namespace daw
