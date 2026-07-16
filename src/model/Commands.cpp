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
    fOld.clear();
    const int group = t->muteGroup;
    if (group > 0) {
        // Mute-group: apply the new state to every member of the group.
        for (Track& o : p.Tracks())
            if (o.muteGroup == group) {
                fOld.push_back({o.id, o.muted});
                o.muted = fNewMuted;
            }
    } else {
        fOld.push_back({t->id, t->muted});
        t->muted = fNewMuted;
    }
    return true;
}

void SetTrackMuteCommand::Undo(Project& p) {
    for (const auto& pr : fOld)
        if (Track* t = p.FindTrack(pr.first))
            t->muted = pr.second;
}

// --- SetTrackMuteGroupCommand -----------------------------------------

bool SetTrackMuteGroupCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOldGroup = t->muteGroup;
    t->muteGroup = fNewGroup < 0 ? 0 : fNewGroup;
    return true;
}

void SetTrackMuteGroupCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->muteGroup = fOldGroup;
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

// --- AddMidiClipCommand -----------------------------------------------

bool AddMidiClipCommand::Do(Project& p) {
    if (fClip.id == kInvalidClipId)
        fClip.id = p.NextClipId();
    return p.AddMidiClip(fTrack, fClip);
}

void AddMidiClipCommand::Undo(Project& p) {
    p.RemoveMidiClip(fTrack, fClip.id);
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

// --- SplitClipCommand -------------------------------------------------

bool SplitClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;
    const Frame start = c->startFrame;
    const Frame end   = c->startFrame + c->lengthFrames;
    if (fAt <= start || fAt >= end) return false;   // must fall strictly inside

    fOldLen     = c->lengthFrames;
    fOldFadeOut = c->fadeOutFrames;

    // Right half: remainder, fade-in cleared, source offset advanced. Allocate
    // the id once (cached) so redo reuses it and never leaks/reassigns ids.
    if (fNewClip == kInvalidClipId) fNewClip = p.NextClipId();
    Clip right = *c;
    right.id            = fNewClip;
    right.startFrame    = fAt;
    right.lengthFrames  = end - fAt;
    right.sourceOffset  = c->sourceOffset + (fAt - start);
    right.fadeInFrames  = 0;

    // Left half: shorten, drop the fade-out (now an interior cut).
    c->lengthFrames  = fAt - start;
    c->fadeOutFrames = 0;

    return p.AddClip(fTrack, right);
}

void SplitClipCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    p.RemoveClip(fTrack, fNewClip);
    if (Clip* c = t->FindClip(fClip)) {
        c->lengthFrames  = fOldLen;
        c->fadeOutFrames = fOldFadeOut;
    }
}

// --- RemoveMidiClipCommand --------------------------------------------

bool RemoveMidiClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* c = t->FindMidiClip(fClip);
    if (!c) return false;
    fRemoved = *c;                    // save for Undo
    return p.RemoveMidiClip(fTrack, fClip);
}

void RemoveMidiClipCommand::Undo(Project& p) {
    p.AddMidiClip(fTrack, fRemoved);  // re-inserted sorted by start
}

// --- MoveMidiClipCommand ----------------------------------------------

bool MoveMidiClipCommand::Do(Project& p) {
    Track* src = p.FindTrack(fTrack);
    if (!src) return false;
    MidiClip* c = src->FindMidiClip(fClip);
    if (!c) return false;
    fOldTrack = fTrack;
    fOldStart = c->startFrame;
    if (fNewTrack == fTrack) {
        c->startFrame = fNewStart < 0 ? 0 : fNewStart;
        std::sort(src->midiClips.begin(), src->midiClips.end(),
                  [](const MidiClip& a, const MidiClip& b) {
                      return a.startFrame < b.startFrame; });
        return true;
    }
    // Cross-track move: pull from src, re-home in the destination.
    MidiClip moved = *c;
    moved.startFrame = fNewStart < 0 ? 0 : fNewStart;
    if (!p.FindTrack(fNewTrack)) return false;
    p.RemoveMidiClip(fTrack, fClip);
    return p.AddMidiClip(fNewTrack, moved);
}

void MoveMidiClipCommand::Undo(Project& p) {
    // Find wherever the clip currently lives and move it back.
    if (fNewTrack != fOldTrack) {
        if (MidiClip* c = p.FindTrack(fNewTrack)
                ? p.FindTrack(fNewTrack)->FindMidiClip(fClip) : nullptr) {
            MidiClip moved = *c;
            moved.startFrame = fOldStart;
            p.RemoveMidiClip(fNewTrack, fClip);
            p.AddMidiClip(fOldTrack, moved);
            return;
        }
    }
    if (Track* t = p.FindTrack(fOldTrack)) {
        if (MidiClip* c = t->FindMidiClip(fClip)) {
            c->startFrame = fOldStart;
            std::sort(t->midiClips.begin(), t->midiClips.end(),
                      [](const MidiClip& a, const MidiClip& b) {
                          return a.startFrame < b.startFrame; });
        }
    }
}

// --- ResizeMidiClipCommand --------------------------------------------

bool ResizeMidiClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* c = t->FindMidiClip(fClip);
    if (!c) return false;
    fOldLen = c->lengthFrames;
    c->lengthFrames = fNewLen < 1 ? 1 : fNewLen;
    return true;
}

void ResizeMidiClipCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        if (MidiClip* c = t->FindMidiClip(fClip))
            c->lengthFrames = fOldLen;
}

// --- SetMidiClipFadeCommand -------------------------------------------

bool SetMidiClipFadeCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* c = t->FindMidiClip(fClip);
    if (!c) return false;
    fOldIn = c->fadeInFrames; fOldOut = c->fadeOutFrames;
    Frame in = fIn < 0 ? 0 : fIn, out = fOut < 0 ? 0 : fOut;
    if (in > c->lengthFrames)  in = c->lengthFrames;
    if (out > c->lengthFrames) out = c->lengthFrames;
    c->fadeInFrames = in; c->fadeOutFrames = out;
    return true;
}

void SetMidiClipFadeCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        if (MidiClip* c = t->FindMidiClip(fClip)) {
            c->fadeInFrames = fOldIn; c->fadeOutFrames = fOldOut;
        }
}

// --- SplitMidiClipCommand ---------------------------------------------

bool SplitMidiClipCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* c = t->FindMidiClip(fClip);
    if (!c) return false;
    const Frame start = c->startFrame;
    const Frame end   = start + c->lengthFrames;
    if (fAt <= start || fAt >= end) return false;   // must fall strictly inside

    fOldLen   = c->lengthFrames;
    fOldNotes = c->notes;
    const Frame rel = fAt - start;   // cut point relative to the clip start

    // Right half: notes at/after the cut, re-based to the new start. Allocate
    // the id once (cached) so redo reuses it and never leaks/reassigns ids.
    if (fRightId == kInvalidClipId) fRightId = p.NextClipId();
    MidiClip right;
    right.id           = fRightId;
    right.startFrame   = fAt;
    right.lengthFrames = end - fAt;
    right.colorIndex   = c->colorIndex;
    for (const MidiNote& n : fOldNotes)
        if (n.startFrame >= rel) {
            MidiNote m = n; m.startFrame -= rel; right.notes.push_back(m);
        }

    // Left half: shorten + drop notes at/after the cut.
    c->lengthFrames = rel;
    c->notes.clear();
    for (const MidiNote& n : fOldNotes)
        if (n.startFrame < rel) c->notes.push_back(n);

    return p.AddMidiClip(fTrack, right);
}

void SplitMidiClipCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    p.RemoveMidiClip(fTrack, fRightId);
    if (MidiClip* c = t->FindMidiClip(fClip)) {
        c->lengthFrames = fOldLen;
        c->notes        = fOldNotes;
    }
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

// --- SetActiveTakeCommand ---------------------------------------------

bool SetActiveTakeCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* target = t->FindClip(fClip);
    if (!target || target->takeGroup == 0) return false;
    const int group = target->takeGroup;
    fOld.clear();
    for (Clip& c : t->clips) {
        if (c.takeGroup != group) continue;
        fOld.push_back({c.id, c.takeActive});
        c.takeActive = (c.id == fClip);
    }
    return true;
}

void SetActiveTakeCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    for (const auto& pr : fOld)
        if (Clip* c = t->FindClip(pr.first))
            c->takeActive = pr.second;
}

// --- SetActiveMidiTakeCommand -----------------------------------------

bool SetActiveMidiTakeCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* target = t->FindMidiClip(fClip);
    if (!target || target->takeGroup == 0) return false;
    const int group = target->takeGroup;
    fOld.clear();
    for (MidiClip& c : t->midiClips) {
        if (c.takeGroup != group) continue;
        fOld.push_back({c.id, c.takeActive});
        c.takeActive = (c.id == fClip);
    }
    return true;
}

void SetActiveMidiTakeCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    for (const auto& pr : fOld)
        if (MidiClip* c = t->FindMidiClip(pr.first))
            c->takeActive = pr.second;
}

// --- SetClipGainCommand -----------------------------------------------

bool SetClipGainCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    Clip* c = t->FindClip(fClip);
    if (!c) return false;
    fOld = c->gain;
    c->gain = fGain < 0.0f ? 0.0f : fGain;
    return true;
}

void SetClipGainCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (t)
        if (Clip* c = t->FindClip(fClip))
            c->gain = fOld;
}

// --- SetSendsCommand --------------------------------------------------

bool SetSendsCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    // Drop invalid sends: a send to nothing, or a self-send (would loop).
    std::vector<Send> clean;
    clean.reserve(fNew.size());
    for (const Send& s : fNew)
        if (s.dest != kInvalidTrackId && s.dest != fTrack)
            clean.push_back(s);
    fNew.swap(clean);
    fOld = t->sends;
    t->sends = fNew;
    return true;
}

void SetSendsCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        t->sends = fOld;
}

// --- MoveTrackCommand -------------------------------------------------

bool MoveTrackCommand::Do(Project& p) {
    const int from = p.IndexOfTrack(fTrack);
    if (from < 0) return false;
    int to = from + fDelta;
    if (to < 0 || to >= (int)p.Tracks().size()) return false;   // at the edge
    fFrom = (size_t)from;
    fTo   = (size_t)to;
    return p.MoveTrack(fFrom, fTo);
}

void MoveTrackCommand::Undo(Project& p) {
    p.MoveTrack(fTo, fFrom);   // move it back
}

// --- SetAutoLaneCommand -----------------------------------------------

bool SetAutoLaneCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    AutomationLane& lane = (fKind == AutoLaneKind::Gain) ? t->gainAuto
                                                         : t->panAuto;
    fOld = lane;
    lane = fNew;
    return true;
}

void SetAutoLaneCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return;
    ((fKind == AutoLaneKind::Gain) ? t->gainAuto : t->panAuto) = fOld;
}

// --- SetFxAutoLaneCommand ---------------------------------------------

bool SetFxAutoLaneCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t || fIdx < 0 || fIdx >= (int)t->fxAuto.size()) return false;
    fOld = t->fxAuto[(size_t)fIdx].lane;
    t->fxAuto[(size_t)fIdx].lane = fNew;
    return true;
}
void SetFxAutoLaneCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (t && fIdx >= 0 && fIdx < (int)t->fxAuto.size())
        t->fxAuto[(size_t)fIdx].lane = fOld;
}

// --- SetFxCommand -----------------------------------------------------

bool SetFxCommand::Do(Project& p) {
    std::vector<EffectDesc>* dst = nullptr;
    if (fMaster) dst = &p.masterFx;
    else if (Track* t = p.FindTrack(fTrack)) dst = &t->fx;
    if (!dst) return false;
    fOld = *dst;
    *dst = fNew;
    return true;
}
void SetFxCommand::Undo(Project& p) {
    if (fMaster) { p.masterFx = fOld; return; }
    if (Track* t = p.FindTrack(fTrack)) t->fx = fOld;
}

// --- Markers ----------------------------------------------------------

bool AddMarkerCommand::Do(Project& p) {
    size_t i = 0;
    while (i < p.markers.size() && p.markers[i].frame < fFrame) i++;
    fIndex = (int)i;
    p.markers.insert(p.markers.begin() + i, Marker{fFrame, fName});
    return true;
}
void AddMarkerCommand::Undo(Project& p) {
    if (fIndex >= 0 && fIndex < (int)p.markers.size())
        p.markers.erase(p.markers.begin() + fIndex);
}

bool RemoveMarkerCommand::Do(Project& p) {
    for (size_t i = 0; i < p.markers.size(); i++)
        if (p.markers[i].frame == fFrame) {
            fRemoved = p.markers[i];
            fIndex   = (int)i;
            p.markers.erase(p.markers.begin() + i);
            return true;
        }
    return false;
}
void RemoveMarkerCommand::Undo(Project& p) {
    if (fIndex >= 0 && fIndex <= (int)p.markers.size())
        p.markers.insert(p.markers.begin() + fIndex, fRemoved);
}

bool RenameMarkerCommand::Do(Project& p) {
    for (Marker& m : p.markers)
        if (m.frame == fFrame) { fOld = m.name; m.name = fNew; return true; }
    return false;
}
void RenameMarkerCommand::Undo(Project& p) {
    for (Marker& m : p.markers)
        if (m.frame == fFrame) { m.name = fOld; return; }
}

// --- SetTrackInputCommand ---------------------------------------------

bool SetTrackInputCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOld = t->input;
    t->input = fNew;
    return true;
}
void SetTrackInputCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack)) t->input = fOld;
}

// --- SetInstrumentCommand ---------------------------------------------

bool SetInstrumentCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOld = t->instrument;
    t->instrument = fNew;
    return true;
}
void SetInstrumentCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack)) t->instrument = fOld;
}

// --- SetMidiClipNotesCommand ------------------------------------------

bool SetMidiClipNotesCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    MidiClip* c = t->FindMidiClip(fClip);
    if (!c) return false;
    fOld = c->notes;
    c->notes = fNew;
    return true;
}
void SetMidiClipNotesCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack))
        if (MidiClip* c = t->FindMidiClip(fClip))
            c->notes = fOld;
}

// --- SetTrackColor / SetTrackHeight -----------------------------------

bool SetTrackColorCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOld = t->colorIndex;
    t->colorIndex = fNew;
    return true;
}
void SetTrackColorCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack)) t->colorIndex = fOld;
}

bool SetTrackHeightCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    fOld = t->height;
    t->height = fNew < 24 ? 24 : fNew;   // match ProjectIO's load floor (24)
    return true;
}
void SetTrackHeightCommand::Undo(Project& p) {
    if (Track* t = p.FindTrack(fTrack)) t->height = fOld;
}

bool FreezeTrackCommand::Do(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t) return false;
    if (fFreeze && t->frozen) return false;    // already frozen
    if (!fFreeze && !t->frozen) return false;  // nothing to unfreeze

    // Snapshot every field this command touches, so Undo restores exactly.
    fSType        = t->type;
    fSClips       = t->clips;
    fSMidi        = t->midiClips;
    fSFx          = t->fx;
    fSGain        = t->gain;
    fSPan         = t->pan;
    fSFrozen      = t->frozen;
    fSFreezeClips = t->freezeClips;
    fSFreezeMidi  = t->freezeMidi;
    fSFreezeFx    = t->freezeFx;
    fSFreezeGain  = t->freezeGain;
    fSFreezePan   = t->freezePan;
    fSFreezeType  = t->freezeType;
    fCaptured     = true;

    if (fFreeze) {
        // Stash the pre-freeze content, then install the rendered clip as the
        // sole content with a flat unity fader and no inserts (all baked in).
        t->freezeClips = t->clips;
        t->freezeMidi  = t->midiClips;
        t->freezeFx    = t->fx;
        t->freezeGain  = t->gain;
        t->freezePan   = t->pan;
        t->freezeType  = t->type;

        t->clips.assign(1, fFrozenClip);
        t->midiClips.clear();
        t->fx.clear();
        t->gain = 1.0f;
        t->pan  = 0.0f;
        t->type = TrackType::Audio;
        t->frozen = true;
    } else {
        // Restore from the stash and drop it.
        t->clips     = t->freezeClips;
        t->midiClips = t->freezeMidi;
        t->fx        = t->freezeFx;
        t->gain      = t->freezeGain;
        t->pan       = t->freezePan;
        t->type      = t->freezeType;
        t->frozen    = false;
        t->freezeClips.clear();
        t->freezeMidi.clear();
        t->freezeFx.clear();
    }
    return true;
}

void FreezeTrackCommand::Undo(Project& p) {
    Track* t = p.FindTrack(fTrack);
    if (!t || !fCaptured) return;
    t->type         = fSType;
    t->clips        = fSClips;
    t->midiClips    = fSMidi;
    t->fx           = fSFx;
    t->gain         = fSGain;
    t->pan          = fSPan;
    t->frozen       = fSFrozen;
    t->freezeClips  = fSFreezeClips;
    t->freezeMidi   = fSFreezeMidi;
    t->freezeFx     = fSFreezeFx;
    t->freezeGain   = fSFreezeGain;
    t->freezePan    = fSFreezePan;
    t->freezeType   = fSFreezeType;
}

} // namespace daw
