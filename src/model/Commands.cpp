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

// --- RemoveTrackCommand -----------------------------------------------

bool RemoveTrackCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fRemoved = *t;
    fIndex   = p.IndexOfTrack(fTrack);
    return p.RemoveTrack(fTrack);
}

void RemoveTrackCommand::Undo(Project& p) {
    if (fIndex >= 0)
        p.InsertTrack((size_t)fIndex, fRemoved);
    else
        p.AddTrack(fRemoved);
}

// --- SetTrackNameCommand ----------------------------------------------

bool SetTrackNameCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldName = t->name;
    t->name  = fNewName;
    return true;
}

void SetTrackNameCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->name = fOldName;
}

// --- SetTrackOutputCommand --------------------------------------------

bool SetTrackOutputCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    // Reject a self-route; the engine's routing resolver rejects cycles too.
    if (fNewOutput == fTrack) return false;
    fOldOutput = t->output;
    t->output = fNewOutput;
    return true;
}

void SetTrackOutputCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->output = fOldOutput;
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

// --- SetTrackMuteCommand ----------------------------------------------

bool SetTrackMuteCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldMuted = t->muted;
    t->muted  = fNewMuted;
    return true;
}

void SetTrackMuteCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->muted = fOldMuted;
}

// --- SetTrackSoloCommand ----------------------------------------------

bool SetTrackSoloCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldSoloed = t->soloed;
    t->soloed  = fNewSoloed;
    return true;
}

void SetTrackSoloCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->soloed = fOldSoloed;
}

// --- AddNoteCommand ---------------------------------------------------

bool AddNoteCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    t->notes.push_back(fNote);
    return true;
}

void AddNoteCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        if (!t->notes.empty())
            t->notes.pop_back();
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

// --- AddEffectCommand -------------------------------------------------

bool AddEffectCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    t->fx.push_back(fDesc);
    return true;
}

void AddEffectCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        if (!t->fx.empty())
            t->fx.pop_back();
}

// --- ClearEffectsCommand ----------------------------------------------

bool ClearEffectsCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOld = t->fx;
    t->fx.clear();
    return true;
}

void ClearEffectsCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->fx = fOld;
}

// --- RemoveClipCommand ------------------------------------------------

bool RemoveClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;
    fRemoved = *c;                    // save for Undo
    return p.RemoveClip(fTrack, fClip);
}

void RemoveClipCommand::Undo(Project& p) {
    p.AddClip(fTrack, fRemoved);      // re-inserted sorted by start
}

// --- RemoveNoteCommand ------------------------------------------------

bool RemoveNoteCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t || fIndex >= t->notes.size()) return false;
    fRemoved = t->notes[fIndex];
    t->notes.erase(t->notes.begin() + fIndex);
    return true;
}

void RemoveNoteCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    size_t i = fIndex <= t->notes.size() ? fIndex : t->notes.size();
    t->notes.insert(t->notes.begin() + i, fRemoved);
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

// --- MoveClipToTrackCommand -------------------------------------------

bool MoveClipToTrackCommand::Do(Project& p) {
    Track* src = p.FindTrack(fSrc);
    Track* dst = p.FindTrack(fDst);
    if (!src || !dst) return false;
    Clip* c = src->FindClip(fClip);
    if (!c) return false;
    fMoved = *c;                       // save original (incl. its start) for undo
    Clip moved = fMoved;
    moved.startFrame = fNewStart;
    if (!p.RemoveClip(fSrc, fClip)) return false;
    return p.AddClip(fDst, moved);     // keeps the same clip id
}

void MoveClipToTrackCommand::Undo(Project& p) {
    p.RemoveClip(fDst, fClip);
    p.AddClip(fSrc, fMoved);           // back on the source track at its old start
}

// --- ResizeClipCommand ------------------------------------------------

bool ResizeClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;
    fOldLength = c->lengthFrames;
    c->lengthFrames = fNewLength > 1 ? fNewLength : 1;
    return true;
}

void ResizeClipCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    if (Clip* c = t->FindClip(fClip))
        c->lengthFrames = fOldLength;
}

// --- SetClipFadeCommand -----------------------------------------------

bool SetClipFadeCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;
    fOldIn = c->fadeInFrames;
    fOldOut = c->fadeOutFrames;
    c->fadeInFrames  = fIn  < 0 ? 0 : fIn;
    c->fadeOutFrames = fOut < 0 ? 0 : fOut;
    return true;
}

void SetClipFadeCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    if (Clip* c = t->FindClip(fClip)) {
        c->fadeInFrames  = fOldIn;
        c->fadeOutFrames = fOldOut;
    }
}

// --- NoteEditCommand --------------------------------------------------

bool NoteEditCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t || fIndex >= t->notes.size()) return false;
    fOld = t->notes[fIndex];
    MidiNote n = fNote;
    if (n.pitch < 0) n.pitch = 0; if (n.pitch > 127) n.pitch = 127;
    if (n.velocity < 1) n.velocity = 1; if (n.velocity > 127) n.velocity = 127;
    if (n.startFrame < 0) n.startFrame = 0;
    if (n.lengthFrames < 1) n.lengthFrames = 1;
    t->notes[fIndex] = n;
    return true;
}

void NoteEditCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (t && fIndex < t->notes.size())
        t->notes[fIndex] = fOld;
}

} // namespace daw
