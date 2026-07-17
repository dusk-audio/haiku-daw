// Command pattern + undo/redo stack.
//
// This is the ONLY sanctioned way to mutate a Project. The UI builds a
// Command and hands it to CommandStack::Execute; nothing else writes to
// the model. Benefits: free undo/redo, a single serialization/audit point,
// and a clean boundary to later marshal edits to the audio thread.
#pragma once

#include "Project.h"

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
        if (!fUndo.empty() && cmd->CoalesceInto(fUndo.back().get())) {
            fRedo.clear();
            return true;
        }
        fUndo.push_back(std::move(cmd));
        // Trim the oldest history past the cap (usually a single entry).
        if (fUndo.size() > kMaxUndoDepth)
            fUndo.erase(fUndo.begin(),
                        fUndo.begin() + (fUndo.size() - kMaxUndoDepth));
        fRedo.clear();
        return true;
    }

    bool CanUndo() const { return !fUndo.empty(); }
    bool CanRedo() const { return !fRedo.empty(); }

    // Drop all history (e.g. after loading a different project, whose edits
    // these commands could no longer correctly reverse).
    void Clear() { fUndo.clear(); fRedo.clear(); }

    bool Undo(Project& p) {
        if (fUndo.empty()) return false;
        std::unique_ptr<Command> cmd = std::move(fUndo.back());
        fUndo.pop_back();
        cmd->Undo(p);
        fRedo.push_back(std::move(cmd));
        return true;
    }

    bool Redo(Project& p) {
        if (fRedo.empty()) return false;
        std::unique_ptr<Command> cmd = std::move(fRedo.back());
        fRedo.pop_back();
        cmd->Do(p);
        fUndo.push_back(std::move(cmd));
        return true;
    }

    std::string UndoName() const {
        return fUndo.empty() ? std::string() : fUndo.back()->Name();
    }
    std::string RedoName() const {
        return fRedo.empty() ? std::string() : fRedo.back()->Name();
    }

private:
    std::vector<std::unique_ptr<Command>> fUndo;
    std::vector<std::unique_ptr<Command>> fRedo;
};

} // namespace daw
