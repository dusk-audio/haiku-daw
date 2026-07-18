// MidiRouting — which live MIDI events belong to which track.
//
// Several keyboards can be connected at once, and each MIDI track names the
// endpoint it records from (Track::input, matched by name so the assignment
// survives endpoint-id reassignment across sessions). MainWindow resolves those
// names to Midi Kit producer ids once, when it opens the input, and hands the
// resulting routes down; everything below then filters purely on ids.
//
// A route also carries the track's MIDI channel filter, so two parts on one
// keyboard (or a splitter sending different channels) can land on different
// tracks.
//
// Kit-free (no Midi Kit / no ids beyond plain ints), header-only, host-testable.
#pragma once

#include "MidiEvent.h"
#include "../model/types.h"

#include <cstdint>
#include <vector>

namespace daw {

// One track's live-input filter. `endpoint` is a Midi Kit producer id, or 0 for
// "any source". `channel` is 1..16, or 0 for "all channels" (matching
// InputSource::channel, where 0 means unfiltered).
struct MidiInputRoute {
    TrackId track    = kInvalidTrackId;
    int32_t endpoint = 0;
    int     channel  = 0;
};

// Does an event reach a track listening on this endpoint/channel?
//
// Both filters are permissive when unset, and an UNTAGGED event (source 0) is
// accepted by every route. That keeps a source that cannot identify itself --
// host tests, loopback, any future non-kit input -- working exactly as it did
// before routing existed, instead of going silently deaf.
inline bool RouteAccepts(int32_t endpoint, int channel, const MidiEvent& e) {
    if (endpoint != 0 && e.source != 0 && e.source != endpoint) return false;
    if (channel  != 0 && (int)e.channel + 1 != channel)          return false;
    return true;
}

inline bool RouteAccepts(const MidiInputRoute& r, const MidiEvent& e) {
    return RouteAccepts(r.endpoint, r.channel, e);
}

// The route for `track`, or a permissive one (any endpoint, all channels) when
// the track has none. Unrouted tracks therefore keep hearing everything, which
// is the pre-routing behaviour.
inline MidiInputRoute RouteFor(const std::vector<MidiInputRoute>& routes,
                               TrackId track) {
    for (const MidiInputRoute& r : routes)
        if (r.track == track) return r;
    MidiInputRoute any;
    any.track = track;
    return any;
}

} // namespace daw
