// Command pattern + undo/redo stack.
//
// This is the ONLY sanctioned way to mutate a Project. The UI builds a
// Command and hands it to CommandStack::Execute; nothing else writes to
// the model. Benefits: free undo/redo, a single serialization/audit point,
// and a clean boundary to later marshal edits to the audio thread.
#pragma once

#include "Project.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

class Command {
public:
    virtual ~Command() = default;

    // Apply the change. Returns false if preconditions failed (e.g. the
    // target track vanished); a failed Do() is not pushed onto the stack.
    virtual bool Do(Project& p) = 0;

    // Exactly reverse a previously-successful Do().
    virtual void Undo(Project& p) = 0;

    // Human-readable label for an "Undo <name>" menu item.
    virtual std::string Name() const = 0;

    // Coalescing: when a just-executed command targets the same thing as the
    // previous one (e.g. successive posts from a slider drag), it can fold its
    // new value into `prev` instead of pushing a separate undo entry — so a
    // whole gesture is one undo step and the stack doesn't flood. Return true
    // if `prev` absorbed this command's result (this command is then discarded,
    // its Do() having already mutated the model). Default: no coalescing.
    virtual bool CoalesceInto(Command* /*prev*/) { return false; }
};

class CommandStack {
public:
    // Serial of a stack position. Serials are never reused: one serial names
    // one exact history state, which is what lets the unsaved-changes test
    // (MainWindow's title and save prompt) survive undo, redo, coalescing and
    // the trimming below. A position index cannot: see MarkSaved().
    using Serial = uint64_t;

    // Cap on retained undo history. Without it a long session accumulates every
    // edit forever (some commands capture large state, e.g. a removed Track's
    // whole clip/note/fx payload). At the cap the oldest entries are dropped —
    // they simply become no-longer-undoable, as in any DAW's bounded history.
    static constexpr size_t kMaxUndoDepth = 256;

    // Execute and, on success, push onto the undo stack. Executing a new
    // command clears the redo stack (standard linear-history semantics).
    bool Execute(std::unique_ptr<Command> cmd, Project& p) {
        if (!cmd || !cmd->Do(p)) return false;
        // Fold into the previous command when they coalesce (one undo step per
        // gesture). The model is already mutated by Do(); we just avoid pushing
        // a redundant entry, keeping the earlier command's captured "old".
        if (!fUndo.empty() && cmd->CoalesceInto(fUndo.back().cmd.get())) {
            // The entry still reverses the whole gesture, but the state it
            // leads to just moved — so it takes a NEW serial. Otherwise a save
            // taken mid-drag would read clean while the drag is still going.
            fUndo.back().serial = fNextSerial++;
            fRedo.clear();
            return true;
        }
        fUndo.push_back(Entry{std::move(cmd), fNextSerial++});
        // Trim the oldest history past the cap (usually a single entry).
        if (fUndo.size() > kMaxUndoDepth) {
            const size_t drop = fUndo.size() - kMaxUndoDepth;
            // The last dropped entry's serial becomes the empty stack's base:
            // undoing everything left lands on the state AFTER that entry, not
            // on the original project, and the base serial says so. (With a
            // position index, an emptied stack would claim to be the original.)
            fBaseSerial = fUndo[drop - 1].serial;
            fUndo.erase(fUndo.begin(), fUndo.begin() + drop);
        }
        fRedo.clear();
        return true;
    }

    bool CanUndo() const { return !fUndo.empty(); }
    bool CanRedo() const { return !fRedo.empty(); }

    // Drop all history (e.g. after loading a different project, whose edits
    // these commands could no longer correctly reverse). The replacement state
    // is on disk by the time a caller does this, so the caller must MarkSaved()
    // once the new project is in place.
    void Clear() { fUndo.clear(); fRedo.clear(); }

    bool Undo(Project& p) {
        if (fUndo.empty()) return false;
        Entry e = std::move(fUndo.back());
        fUndo.pop_back();
        e.cmd->Undo(p);
        fRedo.push_back(std::move(e));
        return true;
    }

    bool Redo(Project& p) {
        if (fRedo.empty()) return false;
        Entry e = std::move(fRedo.back());
        fRedo.pop_back();
        e.cmd->Do(p);
        fUndo.push_back(std::move(e));
        return true;
    }

    std::string UndoName() const {
        return fUndo.empty() ? std::string() : fUndo.back().cmd->Name();
    }
    std::string RedoName() const {
        return fRedo.empty() ? std::string() : fRedo.back().cmd->Name();
    }

    // --- unsaved-changes tracking -----------------------------------------
    // The serial of the state the stack is at right now: the top entry's, or
    // the base serial when the stack is empty.
    Serial CurrentSerial() const {
        return fUndo.empty() ? fBaseSerial : fUndo.back().serial;
    }

    // Record the current state as the one on disk (a save, a load).
    void MarkSaved() { fSavedSerial = CurrentSerial(); }

    bool IsDirty() const { return CurrentSerial() != fSavedSerial; }

private:
    struct Entry {
        std::unique_ptr<Command> cmd;
        Serial                   serial = 0;
    };

    std::vector<Entry> fUndo;
    std::vector<Entry> fRedo;
    Serial fNextSerial  = 1;   // 0 is the base serial, so entries start at 1
    Serial fBaseSerial  = 0;   // the state under the oldest surviving entry
    Serial fSavedSerial = 0;   // what MarkSaved() recorded
};

} // namespace daw
