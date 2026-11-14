#pragma once
#include "model/Project.h"
#include <functional>
#include <vector>

namespace aligna
{
// Snapshot-based undo/redo. states[0] is the initial state; states[cur] is current.
class EditHistory
{
public:
    static constexpr int maxUndoSteps = 200;

    explicit EditHistory(Project initial);

    // Runs edit on a copy; commits only if it returns true.
    bool perform(const juce::String& name, const std::function<bool(Project&)>& edit);
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;
    juce::String undoName() const;
    juce::String redoName() const;
    const Project& current() const;
    void reset(Project p); // clears history, counts as saved
    bool isDirty() const;
    void markSaved();

    // While locked, perform/undo/redo refuse (return false, change nothing).
    void setLocked(bool l) { locked = l; }
    bool isLocked() const { return locked; }
    // Applies fn to every state. No new step; cur/savedIndex/dirty unchanged; no onChange.
    void rewriteAllStates(const std::function<void(Project&)>& fn);

    std::function<void()> onChange;

private:
    struct Entry
    {
        juce::String name; // name of the edit that produced this state
        Project state;
    };
    void notify() const;

    std::vector<Entry> states;
    std::size_t cur = 0;
    bool locked = false;
    long long savedIndex = 0; // -1: saved state no longer reachable
};
} // namespace aligna
