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
};

class CommandStack {
public:
    // Execute and, on success, push onto the undo stack. Executing a new
    // command clears the redo stack (standard linear-history semantics).
    bool Execute(std::unique_ptr<Command> cmd, Project& p) {
        if (!cmd || !cmd->Do(p)) return false;
        fUndo.push_back(std::move(cmd));
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
