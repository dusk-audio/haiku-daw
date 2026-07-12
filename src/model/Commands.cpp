#include "Commands.h"

namespace daw {

// --- AddTrackCommand --------------------------------------------------

bool AddTrackCommand::Do(Project& p) {
    Track t;
    // Reuse the id across redo so history stays stable; only allocate the
    // first time.
    if (fCreatedId == kInvalidTrackId)
        fCreatedId = p.NextTrackId();
    t.id   = fCreatedId;
    t.type = fType;
    t.name = fName;
    return p.AddTrack(t);
}

void AddTrackCommand::Undo(Project& p) {
    p.RemoveTrack(fCreatedId);
}

// --- SetTrackGainCommand ----------------------------------------------

bool SetTrackGainCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldGain = t->gain;
    t->gain  = fNewGain;
    return true;
}

void SetTrackGainCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->gain = fOldGain;
}

// --- SetTrackPanCommand -----------------------------------------------

bool SetTrackPanCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldPan = t->pan;
    t->pan  = fNewPan;
    return true;
}

void SetTrackPanCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->pan = fOldPan;
}

// --- AddClipCommand ---------------------------------------------------

bool AddClipCommand::Do(Project& p) {
    if (fClip.id == kInvalidClipId)
        fClip.id = p.NextClipId();
    return p.AddClip(fTrack, fClip);
}

void AddClipCommand::Undo(Project& p) {
    p.RemoveClip(fTrack, fClip.id);
}

// --- MoveClipCommand --------------------------------------------------

// Moving changes startFrame, which is the sort key. To preserve the
// "clips sorted by start" invariant we remove and re-insert via the
// Project mutators rather than editing in place.
bool MoveClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;

    fOldStart = c->startFrame;
    Clip moved = *c;                 // copy before we invalidate the ptr
    moved.startFrame = fNewStart;
    p.RemoveClip(fTrack, fClip);
    return p.AddClip(fTrack, moved);
}

void MoveClipCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    Clip* c = t->FindClip(fClip);
    if (!c) return;
    Clip moved = *c;
    moved.startFrame = fOldStart;
    p.RemoveClip(fTrack, fClip);
    p.AddClip(fTrack, moved);
}

} // namespace daw
