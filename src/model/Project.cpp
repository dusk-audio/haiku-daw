#include "Project.h"

#include "Sustain.h"

#include <algorithm>

namespace daw {

// --- Track ------------------------------------------------------------

// The playback view of the MIDI content: the same flattening CollectNotes does,
// with the CC64 pedal applied to each region in its own frame base first, so the
// extension is bounded by the region window (and a re-strike of the key cuts it,
// exactly as the live latch does).
std::vector<MidiNote> Track::CollectPlaybackNotes() const {
    std::vector<MidiNote> out;
    for (const MidiClip& c : midiClips) {
        if (c.takeGroup > 0 && !c.takeActive)
            continue;   // inactive loop-record take: silent
        std::vector<MidiNote> region = FlattenMidiClip(c);
        ApplySustain(region, c.events, c.lengthFrames);
        for (MidiNote n : region) {
            n.startFrame += c.startFrame;
            out.push_back(n);
        }
    }
    return out;
}

Clip* Track::FindClip(ClipId id) {
    for (auto& c : clips)
        if (c.id == id) return &c;
    return nullptr;
}

const Clip* Track::FindClip(ClipId id) const {
    for (const auto& c : clips)
        if (c.id == id) return &c;
    return nullptr;
}

MidiClip* Track::FindMidiClip(ClipId id) {
    for (auto& c : midiClips)
        if (c.id == id) return &c;
    return nullptr;
}

const MidiClip* Track::FindMidiClip(ClipId id) const {
    for (const auto& c : midiClips)
        if (c.id == id) return &c;
    return nullptr;
}

// --- Project ----------------------------------------------------------

Track* Project::FindTrack(TrackId id) {
    for (auto& t : fTracks)
        if (t.id == id) return &t;
    return nullptr;
}

const Track* Project::FindTrack(TrackId id) const {
    for (const auto& t : fTracks)
        if (t.id == id) return &t;
    return nullptr;
}

bool Project::AddTrack(const Track& t) {
    if (t.id == kInvalidTrackId || FindTrack(t.id) != nullptr)
        return false;
    fTracks.push_back(t);
    return true;
}

bool Project::InsertTrack(size_t index, const Track& t) {
    if (t.id == kInvalidTrackId || FindTrack(t.id) != nullptr)
        return false;
    if (index > fTracks.size()) index = fTracks.size();
    fTracks.insert(fTracks.begin() + index, t);
    return true;
}

bool Project::RemoveTrack(TrackId id) {
    auto it = std::find_if(fTracks.begin(), fTracks.end(),
                           [&](const Track& t) { return t.id == id; });
    if (it == fTracks.end()) return false;
    fTracks.erase(it);
    return true;
}

int Project::IndexOfTrack(TrackId id) const {
    for (size_t i = 0; i < fTracks.size(); i++)
        if (fTracks[i].id == id) return (int)i;
    return -1;
}

bool Project::MoveTrack(size_t from, size_t to) {
    if (from >= fTracks.size() || to >= fTracks.size() || from == to)
        return false;
    Track t = std::move(fTracks[from]);
    fTracks.erase(fTracks.begin() + from);
    fTracks.insert(fTracks.begin() + to, std::move(t));
    return true;
}

bool Project::AddClip(TrackId track, const Clip& c) {
    Track* t = FindTrack(track);
    if (t == nullptr || c.id == kInvalidClipId || t->FindClip(c.id))
        return false;
    // Keep clips sorted by start position so the engine can walk them in
    // timeline order without re-sorting each playback pass.
    auto it = std::lower_bound(t->clips.begin(), t->clips.end(), c,
        [](const Clip& a, const Clip& b) {
            return a.startFrame < b.startFrame;
        });
    t->clips.insert(it, c);
    return true;
}

bool Project::RemoveClip(TrackId track, ClipId clip) {
    Track* t = FindTrack(track);
    if (t == nullptr) return false;
    auto it = std::find_if(t->clips.begin(), t->clips.end(),
                           [&](const Clip& c) { return c.id == clip; });
    if (it == t->clips.end()) return false;
    t->clips.erase(it);
    return true;
}

bool Project::AddMidiClip(TrackId track, const MidiClip& c) {
    Track* t = FindTrack(track);
    if (t == nullptr || c.id == kInvalidClipId || t->FindMidiClip(c.id))
        return false;
    auto it = std::lower_bound(t->midiClips.begin(), t->midiClips.end(), c,
        [](const MidiClip& a, const MidiClip& b) {
            return a.startFrame < b.startFrame;
        });
    t->midiClips.insert(it, c);
    return true;
}

bool Project::RemoveMidiClip(TrackId track, ClipId clip) {
    Track* t = FindTrack(track);
    if (t == nullptr) return false;
    auto it = std::find_if(t->midiClips.begin(), t->midiClips.end(),
                           [&](const MidiClip& c) { return c.id == clip; });
    if (it == t->midiClips.end()) return false;
    t->midiClips.erase(it);
    return true;
}

} // namespace daw
