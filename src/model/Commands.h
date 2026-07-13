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

// Toggle/set a track's mute. Stores previous for Undo().
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
    bool    fOldMuted = false;
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

// Add a MIDI note to a track. Undo removes the note it appended.
class AddNoteCommand : public Command {
public:
    AddNoteCommand(TrackId track, MidiNote note)
        : fTrack(track), fNote(note) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Add Note"; }

private:
    TrackId  fTrack;
    MidiNote fNote;
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

// Remove the note at `index` on a track. Stores it + its index for Undo().
class RemoveNoteCommand : public Command {
public:
    RemoveNoteCommand(TrackId track, size_t index)
        : fTrack(track), fIndex(index) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Remove Note"; }

private:
    TrackId  fTrack;
    size_t   fIndex;
    MidiNote fRemoved;
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

// Replace a note (move, resize, or velocity edit). Stores old for Undo().
class NoteEditCommand : public Command {
public:
    NoteEditCommand(TrackId track, size_t index, MidiNote note)
        : fTrack(track), fIndex(index), fNote(note) {}

    bool Do(Project& p) override;
    void Undo(Project& p) override;
    std::string Name() const override { return "Edit Note"; }

private:
    TrackId  fTrack;
    size_t   fIndex;
    MidiNote fNote;   // the new value (clamped in Do)
    MidiNote fOld;    // saved in Do()
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

private:
    TrackId           fTrack;
    std::vector<Send> fNew;
    std::vector<Send> fOld;
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

} // namespace daw
