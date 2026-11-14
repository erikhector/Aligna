#include "edit/EditHistory.h"

namespace aligna
{
EditHistory::EditHistory(Project initial)
{
    states.push_back({ {}, std::move(initial) });
}

void EditHistory::notify() const
{
    if (onChange)
        onChange();
}

bool EditHistory::perform(const juce::String& name, const std::function<bool(Project&)>& edit)
{
    if (locked)
        return false;
    Project copy = states[cur].state;
    if (!edit(copy))
        return false;

    states.erase(states.begin() + (std::ptrdiff_t) cur + 1, states.end());
    if (savedIndex > (long long) cur)
        savedIndex = -1;
    states.push_back({ name, std::move(copy) });
    ++cur;

    if (states.size() > (std::size_t) maxUndoSteps + 1)
    {
        states.erase(states.begin());
        --cur;
        if (savedIndex >= 0)
            --savedIndex; // becomes -1 if the saved state was the dropped one
    }
    notify();
    return true;
}

bool EditHistory::undo()
{
    if (locked || !canUndo())
        return false;
    --cur;
    notify();
    return true;
}

bool EditHistory::redo()
{
    if (locked || !canRedo())
        return false;
    ++cur;
    notify();
    return true;
}

bool EditHistory::canUndo() const { return cur > 0; }
bool EditHistory::canRedo() const { return cur + 1 < states.size(); }
juce::String EditHistory::undoName() const { return canUndo() ? states[cur].name : juce::String(); }
juce::String EditHistory::redoName() const { return canRedo() ? states[cur + 1].name : juce::String(); }
const Project& EditHistory::current() const { return states[cur].state; }

void EditHistory::reset(Project p)
{
    states.clear();
    states.push_back({ {}, std::move(p) });
    cur = 0;
    savedIndex = 0;
    notify();
}

void EditHistory::rewriteAllStates(const std::function<void(Project&)>& fn)
{
    for (auto& e : states)
        fn(e.state);
}

bool EditHistory::isDirty() const { return savedIndex != (long long) cur; }
void EditHistory::markSaved() { savedIndex = (long long) cur; }
} // namespace aligna
