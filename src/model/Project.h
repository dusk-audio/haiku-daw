// The Project model: the single source of truth for a session.
//
// Pure data + lookup helpers. It knows nothing about audio buffers, the
// Media Kit, or the UI. All *mutation* goes through the command stack
// (see Command.h) so undo/redo and (later) serialization have one funnel.
#pragma once

#include "types.h"
#include "Effect.h"
#include "Automation.h"
#include "TempoMap.h"
#include "Instrument.h"

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
    float       gain          = 1.0f;   // per-clip linear gain (1.0 = unity)
    // Loop-record take comping: clips sharing a takeGroup (>0) are stacked
    // alternatives; only the one with takeActive sounds. 0 = an ordinary clip.
    int         takeGroup     = 0;
    bool        takeActive    = true;
};

// A single MIDI note. Its position is in frames RELATIVE to the containing
// MidiClip's start (see MidiClip). Pitch is a MIDI note number (60 = middle C).
struct MidiNote {
    int   pitch        = 60;    // 0..127
    int   velocity     = 100;   // 1..127
    Frame startFrame   = 0;     // relative to the clip start
    Frame lengthFrames = 0;
};

// A MIDI region on a MIDI track: a movable/copyable container of notes, the
// MIDI analogue of an audio Clip. Notes are stored clip-relative so the whole
// region moves as a unit. The region is a non-destructive playback window:
// only notes whose start lies within [0, lengthFrames) sound; notes outside
// are kept (e.g. after trimming) but silent. See Track::CollectNotes.
struct MidiClip {
    ClipId                id           = kInvalidClipId;
    Frame                 startFrame   = 0;   // position on the track timeline
    Frame                 lengthFrames = 0;   // region length (the window)
    std::vector<MidiNote> notes;              // clip-relative
    int                   colorIndex   = 0;   // UI tint (0 = inherit track)
};

// An aux send: taps a track's signal and adds `level` * signal into `dest`
// (an aux Bus). Post-fader (the default) taps after the track's fader+fx;
// pre-fader taps the raw pre-fader signal. `dest == kInvalidTrackId` is unused.
struct Send {
    TrackId dest     = kInvalidTrackId;
    float   level    = 1.0f;    // linear send gain
    bool    preFader = false;   // false = post-fader (typical for reverb/delay)
};

// Automation of one effect parameter: fxIndex into the track's fx chain, the
// parameter slot within that effect, and the breakpoint lane (absolute value).
struct FxAutoLane {
    int            fxIndex = 0;
    int            slot    = 0;
    AutomationLane lane;
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
    TrackId           output = kInvalidTrackId;  // routing target; 0 = master
    int               colorIndex = 0;   // index into the UI track-color palette
    int               height     = 72;  // lane height in pixels (UI)
    std::vector<Clip>       clips;      // audio clips, kept sorted by startFrame
    std::vector<MidiClip>   midiClips;  // MIDI regions (Midi tracks)
    std::vector<EffectDesc> fx;       // ordered per-track effect chain
    std::vector<Send>       sends;    // aux sends into buses
    AutomationLane          gainAuto; // volume envelope (absolute gain; empty = static)
    AutomationLane          panAuto;  // pan envelope (absolute pan; empty = static)
    std::vector<FxAutoLane> fxAuto;   // effect-parameter automation lanes
    Instrument              instrument; // synth voice (MIDI tracks)

    Clip*       FindClip(ClipId id);
    const Clip* FindClip(ClipId id) const;
    MidiClip*       FindMidiClip(ClipId id);
    const MidiClip* FindMidiClip(ClipId id) const;

    // Flatten all MIDI regions into absolute-timeline notes for playback:
    // each clip's in-window notes offset by the clip start. Off the RT thread
    // (the engine snapshots the result when building its graph).
    std::vector<MidiNote> CollectNotes() const {
        std::vector<MidiNote> out;
        for (const MidiClip& c : midiClips)
            for (const MidiNote& n : c.notes)
                if (n.startFrame >= 0 && n.startFrame < c.lengthFrames)
                    out.push_back({ n.pitch, n.velocity,
                                    c.startFrame + n.startFrame, n.lengthFrames });
        return out;
    }
};

class Project {
public:
    double        sampleRate = 48000.0;
    double        tempoBPM   = 120.0;   // initial tempo (frame-0 of tempoMap)
    float         masterGain = 1.0f;   // linear, applied to the summed output
    std::vector<EffectDesc> masterFx;  // master bus effect chain (post-sum)
    TimeSignature timeSig;
    TempoMap      tempoMap;            // variable tempo + meter (authoritative)
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
    bool MoveTrack(size_t from, size_t to);           // reorder within the list
    bool AddClip(TrackId track, const Clip& c);
    bool RemoveClip(TrackId track, ClipId clip);
    bool AddMidiClip(TrackId track, const MidiClip& c);
    bool RemoveMidiClip(TrackId track, ClipId clip);

private:
    std::vector<Track> fTracks;
    TrackId            fLastTrackId = kInvalidTrackId;
    ClipId             fLastClipId  = kInvalidClipId;
};

} // namespace daw
