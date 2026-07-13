// The Project model: the single source of truth for a session.
//
// Pure data + lookup helpers. It knows nothing about audio buffers, the
// Media Kit, or the UI. All *mutation* goes through the command stack
// (see Command.h) so undo/redo and (later) serialization have one funnel.
#pragma once

#include "types.h"
#include "Effect.h"

#include <string>
#include <vector>

namespace daw {

// A region of source material placed on a track's timeline. Non-destructive:
// a clip references a source file + an offset into it; it never owns audio.
struct Clip {
    ClipId      id           = kInvalidClipId;
    Frame       startFrame   = 0;   // position on the track timeline
    Frame       lengthFrames = 0;
    Frame       sourceOffset = 0;   // where in the source playback begins
    std::string sourcePath;         // audio/MIDI file this clip plays
    Frame       fadeInFrames  = 0;
    Frame       fadeOutFrames = 0;
};

// A single MIDI note placed on a track's timeline. Positions are in project
// (timeline) frames, same unit as audio clips, so audio and MIDI share the
// one transport clock. Pitch is a MIDI note number (60 = middle C).
struct MidiNote {
    int   pitch        = 60;    // 0..127
    int   velocity     = 100;   // 1..127
    Frame startFrame   = 0;
    Frame lengthFrames = 0;
};

struct Track {
    TrackId           id    = kInvalidTrackId;
    TrackType         type  = TrackType::Audio;
    std::string       name;
    float             gain  = 1.0f;   // linear, 1.0 = unity
    float             pan   = 0.0f;   // -1 = L, 0 = center, +1 = R
    bool              muted = false;
    bool              soloed = false;
    bool              armed  = false;
    std::vector<Clip>       clips;    // audio clips, kept sorted by startFrame
    std::vector<MidiNote>   notes;    // MIDI notes (Midi tracks)
    std::vector<EffectDesc> fx;       // ordered per-track effect chain

    Clip*       FindClip(ClipId id);
    const Clip* FindClip(ClipId id) const;
};

class Project {
public:
    double        sampleRate = 48000.0;
    double        tempoBPM   = 120.0;
    float         masterGain = 1.0f;   // linear, applied to the summed output
    TimeSignature timeSig;
    Transport     transport;

    const std::vector<Track>& Tracks() const { return fTracks; }
    std::vector<Track>&       Tracks()       { return fTracks; }

    Track*       FindTrack(TrackId id);
    const Track* FindTrack(TrackId id) const;

    // ID allocation. Commands call these so ids are unique across the
    // session lifetime (and never reused, which keeps undo history sane).
    TrackId NextTrackId() { return ++fLastTrackId; }
    ClipId  NextClipId()  { return ++fLastClipId; }

    // Reset to an empty session (used before loading a project from disk).
    void Clear() {
        fTracks.clear();
        fLastTrackId = kInvalidTrackId;
        fLastClipId  = kInvalidClipId;
    }

    // After loading tracks/clips with explicit ids, bump the allocators so
    // newly created tracks/clips never reuse a loaded id.
    void ReserveIds(TrackId maxTrack, ClipId maxClip) {
        if (maxTrack > fLastTrackId) fLastTrackId = maxTrack;
        if (maxClip  > fLastClipId)  fLastClipId  = maxClip;
    }

    // Direct mutators — intended to be called by Command objects, not by
    // the UI. Return success so commands can assert their preconditions.
    bool AddTrack(const Track& t);
    bool InsertTrack(size_t index, const Track& t);   // for faithful undo
    bool RemoveTrack(TrackId id);
    int  IndexOfTrack(TrackId id) const;              // -1 if not found
    bool AddClip(TrackId track, const Clip& c);
    bool RemoveClip(TrackId track, ClipId clip);

private:
    std::vector<Track> fTracks;
    TrackId            fLastTrackId = kInvalidTrackId;
    ClipId             fLastClipId  = kInvalidClipId;
};

} // namespace daw
