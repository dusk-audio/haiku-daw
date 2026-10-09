// Concrete edit commands. Each captures enough state in Do() to reverse
// itself exactly in Undo(). This starter set proves the pattern across the
// three mutation shapes: create, delete-implied, and property change.
#pragma once

#include "Command.h"

namespace daw {

// Add a new track. Allocates a fresh id via Project::NextTrackId in Do()
// and stores it so Undo() can remove exactly this track.
class AddTrackCommand : public Command {
public:
    AddTrackCommand(TrackType type, std::string name)
        : fType(type), fName(std::move(name)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add Track"; }

    TrackId CreatedId() const { return fCreatedId; }

private:
    TrackType   fType;
    std::string fName;
    TrackId     fCreatedId = kInvalidTrackId;
};

// Remove a track. Stores the whole track + its index so Undo restores it in
// place.
class RemoveTrackCommand : public Command {
public:
    explicit RemoveTrackCommand(TrackId track) : fTrack(track) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Remove Track"; }

private:
    TrackId fTrack;
    Track   fRemoved;    // saved in Do()
    int     fIndex = -1;
};

// Rename a track. Stores the previous name for Undo().
class SetTrackNameCommand : public Command {
public:
    SetTrackNameCommand(TrackId track, std::string name)
        : fTrack(track), fNewName(std::move(name)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Rename Track"; }

private:
    TrackId     fTrack;
    std::string fNewName;
    std::string fOldName;
};

// Route a track's output to a bus (or master = kInvalidTrackId). Stores old.
class SetTrackOutputCommand : public Command {
public:
    SetTrackOutputCommand(TrackId track, TrackId output)
        : fTrack(track), fNewOutput(output) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Route Track"; }

private:
    TrackId fTrack;
    TrackId fNewOutput;
    TrackId fOldOutput = kInvalidTrackId;
};

// Change a track's linear gain. Stores the previous value for Undo().
class SetTrackGainCommand : public Command {
public:
    SetTrackGainCommand(TrackId track, float gain)
        : fTrack(track), fNewGain(gain) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Track Gain"; }

private:
    TrackId fTrack;
    float   fNewGain;
    float   fOldGain = 1.0f;
};

// Change a track's pan (-1 left .. +1 right). Stores previous for Undo().
class SetTrackPanCommand : public Command {
public:
    SetTrackPanCommand(TrackId track, float pan)
        : fTrack(track), fNewPan(pan) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Track Pan"; }

private:
    TrackId fTrack;
    float   fNewPan;
    float   fOldPan = 0.0f;
};

// Toggle/set a track's mute. If the track belongs to a mute group (muteGroup>0)
// the new state is applied to every member of that group. Stores each affected
// track's previous state for Undo().
class SetTrackMuteCommand : public Command {
public:
    SetTrackMuteCommand(TrackId track, bool muted)
        : fTrack(track), fNewMuted(muted) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Mute Track"; }

private:
    TrackId fTrack;
    bool    fNewMuted;
    std::vector<std::pair<TrackId, bool>> fOld;   // affected tracks' prior mute
};

// Assign a track's mute group (0 = none). Stores the old group for Undo().
class SetTrackMuteGroupCommand : public Command {
public:
    SetTrackMuteGroupCommand(TrackId track, int group)
        : fTrack(track), fNewGroup(group) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Mute Group"; }

private:
    TrackId fTrack;
    int     fNewGroup;
    int     fOldGroup = 0;
};

// Set a track's solo flag. Stores previous for Undo().
class SetTrackSoloCommand : public Command {
public:
    SetTrackSoloCommand(TrackId track, bool soloed)
        : fTrack(track), fNewSoloed(soloed) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Solo Track"; }

private:
    TrackId fTrack;
    bool    fNewSoloed;
    bool    fOldSoloed = false;
};

// Set a track's record input source (MIDI endpoint / audio input). Discrete
// choice from the header input picker; stores the old source for Undo().
class SetTrackInputCommand : public Command {
public:
    SetTrackInputCommand(TrackId track, InputSource input)
        : fTrack(track), fNew(std::move(input)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Track Input"; }

private:
    TrackId     fTrack;
    InputSource fNew;
    InputSource fOld;
};

// --- Markers (named timeline positions) -------------------------------

// Add a named marker (Project::markers stays sorted by frame). Undo removes it.
class AddMarkerCommand : public Command {
public:
    AddMarkerCommand(Frame frame, std::string name)
        : fFrame(frame), fName(std::move(name)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add Marker"; }
private:
    Frame       fFrame;
    std::string fName;
    int         fIndex = -1;   // where it landed (for undo)
};

// Remove the marker at `frame`. Stores it for undo. Pass `name` to disambiguate
// when two markers share a frame (else the first at that frame is removed).
class RemoveMarkerCommand : public Command {
public:
    explicit RemoveMarkerCommand(Frame frame) : fFrame(frame) {}
    RemoveMarkerCommand(Frame frame, std::string name)
        : fFrame(frame), fName(std::move(name)), fHasName(true) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Remove Marker"; }
private:
    Frame       fFrame;
    std::string fName;
    bool        fHasName = false;
    Marker      fRemoved;
    int         fIndex = -1;
};

// Rename the marker at `frame`. Stores the old name for undo. Pass `oldName` to
// disambiguate when two markers share a frame (else the first is renamed).
class RenameMarkerCommand : public Command {
public:
    RenameMarkerCommand(Frame frame, std::string name)
        : fFrame(frame), fNew(std::move(name)) {}
    RenameMarkerCommand(Frame frame, std::string oldName, std::string newName)
        : fFrame(frame), fNew(std::move(newName)),
          fOldMatch(std::move(oldName)), fHasMatch(true) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Rename Marker"; }
private:
    Frame       fFrame;
    std::string fNew, fOld, fOldMatch;
    bool        fHasMatch = false;
    int         fIndex = -1;
};

// Add a MIDI region (clip) to a track. Allocates a clip id in Do() if unset;
// Undo() removes it. CreatedId() gives the id after Do() (for the UI).
class AddMidiClipCommand : public Command {
public:
    AddMidiClipCommand(TrackId track, MidiClip clip)
        : fTrack(track), fClip(std::move(clip)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add MIDI Clip"; }
    ClipId CreatedId() const { return fClip.id; }

private:
    TrackId  fTrack;
    MidiClip fClip;
};

// Add a clip to a track. Allocates a clip id in Do(); Undo() removes it.
class AddClipCommand : public Command {
public:
    AddClipCommand(TrackId track, Clip clip)
        : fTrack(track), fClip(std::move(clip)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add Clip"; }

    ClipId CreatedId() const { return fClip.id; }

private:
    TrackId fTrack;
    Clip    fClip;   // fClip.id is filled in during Do()
};

// Append an effect to a track's chain. Undo removes the one it added.
class AddEffectCommand : public Command {
public:
    AddEffectCommand(TrackId track, EffectDesc desc)
        : fTrack(track), fDesc(desc) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add Effect"; }

private:
    TrackId    fTrack;
    EffectDesc fDesc;
};

// Remove all effects from a track. Stores the old chain for Undo().
class ClearEffectsCommand : public Command {
public:
    explicit ClearEffectsCommand(TrackId track) : fTrack(track) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Clear Effects"; }

private:
    TrackId                 fTrack;
    std::vector<EffectDesc> fOld;
};

// Remove a clip from a track. Stores the removed clip for Undo().
class RemoveClipCommand : public Command {
public:
    RemoveClipCommand(TrackId track, ClipId clip)
        : fTrack(track), fClip(clip) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Remove Clip"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    Clip    fRemoved;   // saved in Do() for Undo()
};

// Split a clip at a timeline frame into two adjacent clips. The left keeps the
// clip's id (shortened, fade-out cleared); the right is a new clip covering the
// remainder (fade-in cleared, source offset advanced). No-op unless the split
// falls strictly inside the clip.
class SplitClipCommand : public Command {
public:
    SplitClipCommand(TrackId track, ClipId clip, Frame at)
        : fTrack(track), fClip(clip), fAt(at) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Split Clip"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fAt;
    ClipId  fNewClip     = kInvalidClipId;   // right half (allocated in Do)
    Frame   fOldLen      = 0;                 // left clip's original length
    Frame   fOldFadeOut  = 0;                 // left clip's original fade-out
};

// Remove a MIDI region from a track. Stores it for Undo().
class RemoveMidiClipCommand : public Command {
public:
    RemoveMidiClipCommand(TrackId track, ClipId clip)
        : fTrack(track), fClip(clip) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Remove MIDI Clip"; }

private:
    TrackId  fTrack;
    ClipId   fClip;
    MidiClip fRemoved;
};

// Move a clip to a new start position. Stores the old position for Undo().
class MoveClipCommand : public Command {
public:
    MoveClipCommand(TrackId track, ClipId clip, Frame newStart)
        : fTrack(track), fClip(clip), fNewStart(newStart) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Move Clip"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fNewStart;
    Frame   fOldStart = 0;
};

// Move a clip to a different track (and new start). Stores enough to reverse.
class MoveClipToTrackCommand : public Command {
public:
    MoveClipToTrackCommand(TrackId srcTrack, ClipId clip, TrackId dstTrack,
                           Frame newStart)
        : fSrc(srcTrack), fClip(clip), fDst(dstTrack), fNewStart(newStart) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Move Clip To Track"; }

private:
    TrackId fSrc, fDst;
    ClipId  fClip;
    Frame   fNewStart;
    Clip    fMoved;      // the clip as it was on the source track (saved in Do)
};

// Change a clip's length (right-edge trim/extend). Stores old for Undo().
class ResizeClipCommand : public Command {
public:
    ResizeClipCommand(TrackId track, ClipId clip, Frame newLength)
        : fTrack(track), fClip(clip), fNewLength(newLength) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Resize Clip"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fNewLength;
    Frame   fOldLength = 0;
};

// Set a clip's fade-in / fade-out lengths (frames). Stores old for Undo().
class SetClipFadeCommand : public Command {
public:
    SetClipFadeCommand(TrackId track, ClipId clip, Frame fadeIn, Frame fadeOut)
        : fTrack(track), fClip(clip), fIn(fadeIn), fOut(fadeOut) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Clip Fade"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fIn, fOut;
    Frame   fOldIn = 0, fOldOut = 0;
};

// Make one clip the active take in its take group (others in the group go
// inactive). No-op for an ordinary clip (takeGroup 0). Stores prior states.
class SetActiveTakeCommand : public Command {
public:
    SetActiveTakeCommand(TrackId track, ClipId clip)
        : fTrack(track), fClip(clip) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Select Take"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    std::vector<std::pair<ClipId, bool>> fOld;   // group members' prior active
};

// Make one MIDI region the active take in its take group (others go inactive).
// The MIDI analogue of SetActiveTakeCommand. No-op for an ordinary region.
class SetActiveMidiTakeCommand : public Command {
public:
    SetActiveMidiTakeCommand(TrackId track, ClipId clip)
        : fTrack(track), fClip(clip) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Select Take"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    std::vector<std::pair<ClipId, bool>> fOld;
};

// Set a clip's per-clip linear gain. Stores the old value for Undo().
class SetClipGainCommand : public Command {
public:
    SetClipGainCommand(TrackId track, ClipId clip, float gain)
        : fTrack(track), fClip(clip), fGain(gain) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Clip Gain"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    float   fGain;
    float   fOld = 1.0f;
};

// Move a MIDI region to a new start position (and optionally another track).
// Stores the old track+position for Undo(). Coalesces during a drag.
class MoveMidiClipCommand : public Command {
public:
    MoveMidiClipCommand(TrackId track, ClipId clip, Frame newStart,
                        TrackId newTrack = kInvalidTrackId)
        : fTrack(track), fClip(clip), fNewStart(newStart),
          fNewTrack(newTrack == kInvalidTrackId ? track : newTrack) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Move MIDI Clip"; }
    // No coalescing: the timeline commits one command per drag gesture (on
    // mouse-up), so coalescing would merge two separate drags into one undo.

private:
    TrackId fTrack, fNewTrack;
    ClipId  fClip;
    Frame   fNewStart;
    TrackId fOldTrack = kInvalidTrackId;
    Frame   fOldStart = 0;
};

// Resize a MIDI region's window length (non-destructive: notes outside are kept
// but silent). Stores the old length for Undo(). Coalesces during a drag.
// Trim a region's FRONT: dragging its left edge moves the start and adjusts the
// length so the right edge stays put. One command rather than a move plus a
// resize, so the gesture is a single undo step and the region can never be
// caught mid-trim in a half-applied state.
//
// For an AUDIO clip the read offset into the source moves by the same delta --
// that is what makes it a trim rather than a slide, and forgetting it is how a
// front-trim ends up shifting the audio against the timeline. MIDI regions have
// no source offset and pass 0; their equivalent is the note/controller rebase
// below, since MIDI content is stored clip-relative.
class TrimClipFrontCommand : public Command {
public:
    TrimClipFrontCommand(TrackId track, ClipId clip, bool midi,
                         Frame newStart, Frame newLength, Frame newSourceOffset)
        : fTrack(track), fClip(clip), fMidi(midi), fNewStart(newStart),
          fNewLen(newLength), fNewSrc(newSourceOffset) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Trim Clip"; }

private:
    TrackId fTrack;
    ClipId  fClip;
    bool    fMidi;
    Frame   fNewStart, fNewLen, fNewSrc;
    Frame   fOldStart = 0, fOldLen = 0, fOldSrc = 0;
    // MIDI only. Do() rebases every note/controller by the inverse of the
    // start delta so the music keeps its absolute timeline position; content
    // pushed outside the window keeps its (possibly negative) offset, because
    // the region is a non-destructive window and widening it must bring the
    // content back. Rebasing is not reversible by arithmetic alone once the
    // content can leave the window, so Undo restores these snapshots verbatim
    // (same pattern as SplitMidiClipCommand::fOldNotes).
    std::vector<MidiNote>      fOldNotes;
    std::vector<MidiClipEvent> fOldEvents;
};

class ResizeMidiClipCommand : public Command {
public:
    ResizeMidiClipCommand(TrackId track, ClipId clip, Frame newLength)
        : fTrack(track), fClip(clip), fNewLen(newLength) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Resize MIDI Clip"; }
    // No coalescing (one command per drag gesture; see MoveMidiClipCommand).

private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fNewLen;
    Frame   fOldLen = 0;
};

// Set a MIDI region's velocity fade-in / fade-out (clip-relative frames).
// Coalesces during a drag; stores the old fades for Undo.
class SetMidiClipFadeCommand : public Command {
public:
    SetMidiClipFadeCommand(TrackId track, ClipId clip, Frame fadeIn, Frame fadeOut)
        : fTrack(track), fClip(clip), fIn(fadeIn), fOut(fadeOut) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set MIDI Fade"; }
    // No coalescing (one command per drag gesture; see MoveMidiClipCommand).
private:
    TrackId fTrack;
    ClipId  fClip;
    Frame   fIn, fOut;
    Frame   fOldIn = 0, fOldOut = 0;
};

// Split a MIDI region at absolute frame `at` into two regions: the left half
// keeps notes starting before the cut, the right half (new id) gets the rest,
// re-based to its own start. Undo restores the original region (length + notes)
// and removes the right half.
class SplitMidiClipCommand : public Command {
public:
    SplitMidiClipCommand(TrackId track, ClipId clip, Frame at)
        : fTrack(track), fClip(clip), fAt(at) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Split MIDI Clip"; }

private:
    TrackId               fTrack;
    ClipId                fClip;
    Frame                 fAt;
    Frame                 fOldLen = 0;
    std::vector<MidiNote> fOldNotes;             // for undo
    ClipId                fRightId = kInvalidClipId;
};

// Freeze or unfreeze a track. Freezing bakes the track's clips/notes through
// its fader + effect chain into one rendered audio clip (supplied by the UI,
// which does the offline render) and stashes the pre-freeze content on the
// track; unfreezing restores it. Either direction snapshots every field it
// touches so Undo reverses exactly. `freeze=false` requires the track to be
// frozen (else Do fails and nothing is pushed).
class FreezeTrackCommand : public Command {
public:
    // Freeze: pass freeze=true + the rendered clip (path/length filled in).
    // Unfreeze: pass freeze=false; frozenClip is ignored.
    FreezeTrackCommand(TrackId track, bool freeze, Clip frozenClip = Clip{})
        : fTrack(track), fFreeze(freeze), fFrozenClip(std::move(frozenClip)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return fFreeze ? "Freeze Track"
                                                       : "Unfreeze Track"; }

private:
    TrackId   fTrack;
    bool      fFreeze;
    Clip      fFrozenClip;
    bool      fCaptured = false;   // is the snapshot below valid (Do ran)?
    // Full snapshot of the mutated fields, for exact Undo.
    TrackType               fSType = TrackType::Audio;
    std::vector<Clip>       fSClips;
    std::vector<MidiClip>   fSMidi;
    std::vector<EffectDesc> fSFx;
    float                   fSGain = 1.0f, fSPan = 0.0f;
    bool                    fSFrozen = false;
    std::vector<Clip>       fSFreezeClips;
    std::vector<MidiClip>   fSFreezeMidi;
    std::vector<EffectDesc> fSFreezeFx;
    float                   fSFreezeGain = 1.0f, fSFreezePan = 0.0f;
    TrackType               fSFreezeType = TrackType::Audio;
};

// Move a track up (-1) or down (+1) in the track list. Clamped; a no-op move
// (already at the edge) reports failure so it doesn't hit the undo stack.
class MoveTrackCommand : public Command {
public:
    MoveTrackCommand(TrackId track, int delta)
        : fTrack(track), fDelta(delta) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Move Track"; }

private:
    TrackId fTrack;
    int     fDelta;
    size_t  fFrom = 0, fTo = 0;   // resolved indices (for undo)
};

// Replace a track's whole aux-send list. The sends editor edits a snapshot and
// applies it wholesale (same pattern as the effects/mixer windows). Stores the
// old list for Undo().
class SetSendsCommand : public Command {
public:
    SetSendsCommand(TrackId track, std::vector<Send> sends)
        : fTrack(track), fNew(std::move(sends)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Sends"; }
    bool CoalesceInto(Command* prev) override {
        auto* p = dynamic_cast<SetSendsCommand*>(prev);
        if (!p || p->fTrack != fTrack) return false;
        p->fNew = fNew;   // absorb latest (its editor posts continuously)
        return true;
    }

private:
    TrackId           fTrack;
    std::vector<Send> fNew;
    std::vector<Send> fOld;
};

// Replace a track's effect chain, or the master chain (master == true). One
// undo step per editor gesture (the effects editor posts on mouse-up).
class SetFxCommand : public Command {
public:
    SetFxCommand(TrackId track, bool master, std::vector<EffectDesc> fx)
        : fTrack(track), fMaster(master), fNew(std::move(fx)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Effects"; }
private:
    TrackId                 fTrack;
    bool                    fMaster;
    std::vector<EffectDesc> fNew, fOld;
};

// Toggle one insert's bypass on a track chain. SetFxCommand already carries
// bypass/mix (it replaces the whole chain of descriptors), but a bypass click
// is not a chain edit: this keeps the Edit menu reading "Bypass Effect" instead
// of "Edit Effects", and keeps the undo entry small. Deliberately does NOT
// coalesce — each toggle is a discrete decision the user undoes one at a time,
// unlike a knob drag.
class SetFxBypassCommand : public Command {
public:
    SetFxBypassCommand(TrackId track, int fxIndex, bool bypassed)
        : fTrack(track), fIndex(fxIndex), fNew(bypassed) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override {
        return fNew ? "Bypass Effect" : "Enable Effect";
    }
private:
    TrackId fTrack;
    int     fIndex;
    bool    fNew;
    bool    fOld = false;
};

// Set parameter values on ONE insert of a track chain, or of the master chain
// (master == true).
//
// A native LV2 editor commits through this: its control-port writes go live to
// the audio through kMsgFxLive while the knob moves, and one of these lands
// once the gesture goes quiet so the model keeps what is already audible --
// without it the next engine rebuild (any structural edit, and every play)
// would restore the stale value and the GUI's edit would silently undo itself.
//
// Narrow on purpose: SetFxCommand replaces the whole chain, so routing a knob
// through it would make every gesture an "Edit Effects" undo step carrying a
// full-chain payload. One command can carry several slots, because a plugin's
// own preset load writes many at once.
class SetFxParamCommand : public Command {
public:
    struct SlotValue {
        int   slot  = 0;
        float value = 0.0f;
    };

    SetFxParamCommand(TrackId track, bool master, int fxIndex,
                      std::vector<SlotValue> values)
        : fTrack(track), fMaster(master), fIndex(fxIndex),
          fNew(std::move(values)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Effect Parameter"; }
private:
    TrackId                fTrack;
    bool                   fMaster;
    int                    fIndex;
    std::vector<SlotValue> fNew;
    std::vector<SlotValue> fOld;   // only the slots fNew actually touched
};

// Set a MIDI track's voice. Coalesces (its editor's native sliders post
// continuously) so a slider drag is one undo step.
class SetInstrumentCommand : public Command {
public:
    SetInstrumentCommand(TrackId track, InstrumentDesc inst)
        : fTrack(track), fNew(std::move(inst)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Instrument"; }
    bool CoalesceInto(Command* prev) override {
        auto* p = dynamic_cast<SetInstrumentCommand*>(prev);
        if (!p || p->fTrack != fTrack) return false;
        // Don't fold a change of voice KIND (or of soundfont file) into a
        // slider drag: swapping the instrument is a deliberate step the user
        // expects to undo on its own, not part of the tweak before it.
        if (p->fNew.type != fNew.type || p->fNew.path != fNew.path
            || p->fNew.sf2Preset != fNew.sf2Preset)
            return false;
        p->fNew = fNew;
        return true;
    }
private:
    TrackId        fTrack;
    InstrumentDesc fNew, fOld;
};

// Replace one MIDI region's note list (piano-roll edits). One step per gesture
// (the piano roll posts on mouse-up / add / delete). Notes are clip-relative.
class SetMidiClipNotesCommand : public Command {
public:
    SetMidiClipNotesCommand(TrackId track, ClipId clip,
                            std::vector<MidiNote> notes)
        : fTrack(track), fClip(clip), fNew(std::move(notes)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Notes"; }
private:
    TrackId               fTrack;
    ClipId                fClip;
    std::vector<MidiNote> fNew, fOld;
    // The region GROWS to cover notes drawn past its end. A note outside the
    // region is not played (the engine renders within region bounds), so
    // leaving the length alone silently discards what the user just drew. The
    // SMF importer already does exactly this; the interactive path did not.
    // Saved so Undo restores the old length as well as the old notes.
    Frame                 fOldLen = 0;
};

// Replace one MIDI region's note list with the result of a named piano-roll
// transform (quantize / humanize / legato / transpose / velocity). The
// transform runs in the roll, on its snapshot, and the RESULT is what travels
// (see MidiOps.h for why the parameters do not); this records it. `name` is
// carried so Undo reads "Undo Quantize" rather than "Undo Edit Notes".
//
// Unlike SetMidiClipNotesCommand this does NOT grow the region. A transform
// clamps its own output to the window, so growth could only come from content
// that was already outside it — and a transform must not resize the region as
// a side effect. Its Undo therefore restores notes only, never a length.
class ApplyMidiOpCommand : public Command {
public:
    ApplyMidiOpCommand(TrackId track, ClipId clip, std::vector<MidiNote> notes,
                       std::string name)
        : fTrack(track), fClip(clip), fNew(std::move(notes)),
          fName(std::move(name)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return fName; }
private:
    TrackId               fTrack;
    ClipId                fClip;
    std::vector<MidiNote> fNew, fOld;
    std::string           fName;
};

// Replace a MIDI region's controller events (CC / pitch-bend / program /
// pressure), leaving its notes untouched — the mirror of
// SetMidiClipNotesCommand, which touches only `notes`. The piano roll's CC lane
// edits a snapshot and posts it wholesale; one undo step per gesture.
class SetMidiClipEventsCommand : public Command {
public:
    SetMidiClipEventsCommand(TrackId track, ClipId clip,
                             std::vector<MidiClipEvent> events)
        : fTrack(track), fClip(clip), fNew(std::move(events)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Controllers"; }
private:
    TrackId                    fTrack;
    ClipId                     fClip;
    std::vector<MidiClipEvent> fNew, fOld;
};

// Set a track's color index / lane height (view props; discrete, undoable).
class SetTrackColorCommand : public Command {
public:
    SetTrackColorCommand(TrackId track, int colorIndex)
        : fTrack(track), fNew(colorIndex) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Track Color"; }
private:
    TrackId fTrack; int fNew; int fOld = 0;
};
class SetTrackHeightCommand : public Command {
public:
    SetTrackHeightCommand(TrackId track, int height)
        : fTrack(track), fNew(height) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Set Track Height"; }
    bool CoalesceInto(Command* prev) override {
        auto* p = dynamic_cast<SetTrackHeightCommand*>(prev);
        if (!p || p->fTrack != fTrack) return false;
        p->fNew = fNew;
        return true;
    }
private:
    TrackId fTrack; int fNew; int fOld = 72;
};

// Replace a track's whole gain or pan automation lane. The timeline edits a
// local copy during a drag gesture and applies the result on mouse-up (one
// undo entry per gesture; avoids breakpoint-index churn). Stores the old lane.
class SetAutoLaneCommand : public Command {
public:
    SetAutoLaneCommand(TrackId track, AutoLaneKind kind, AutomationLane lane)
        : fTrack(track), fKind(kind), fNew(std::move(lane)) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Automation"; }

private:
    TrackId        fTrack;
    AutoLaneKind   fKind;
    AutomationLane fNew;
    AutomationLane fOld;
};

// Replace one effect-parameter automation lane (Track.fxAuto[index].lane). One
// undo step per timeline gesture (same restore-on-mouse-up pattern).
class SetFxAutoLaneCommand : public Command {
public:
    SetFxAutoLaneCommand(TrackId track, int fxAutoIndex, AutomationLane lane)
        : fTrack(track), fIdx(fxAutoIndex), fNew(std::move(lane)) {}
    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Fx Automation"; }
private:
    TrackId        fTrack;
    int            fIdx;
    AutomationLane fNew, fOld;
};

// Group several commands into one undoable step (multi-select edits: delete /
// move / paste many clips at once). Do() applies them in order; Undo() reverses
// in the opposite order. Do() fails only if the group is empty or the first
// sub-command fails (so a wholly no-op group isn't pushed).
class MacroCommand : public Command {
public:
    explicit MacroCommand(std::string name) : fName(std::move(name)) {}

    void Add(std::unique_ptr<Command> c) { fCmds.push_back(std::move(c)); }
    bool Empty() const { return fCmds.empty(); }

    bool Do(Project& p) override {
        if (fCmds.empty()) return false;
        fDone.assign(fCmds.size(), false);
        bool any = false;
        for (size_t i = 0; i < fCmds.size(); i++) {
            fDone[i] = fCmds[i]->Do(p);
            any |= fDone[i];
        }
        return any;
    }
    void Undo(Project& p) override {
        // Reverse only the sub-commands whose Do() actually succeeded — a
        // failed Do() captured no rollback state, so its Undo() must not run.
        for (size_t i = fCmds.size(); i-- > 0; )
            if (i < fDone.size() && fDone[i])
                fCmds[i]->Undo(p);
    }
    std::string Name() const override { return fName; }

private:
    std::string                           fName;
    std::vector<std::unique_ptr<Command>> fCmds;
    std::vector<bool>                     fDone;   // which sub-Do()s succeeded
};

} // namespace daw
