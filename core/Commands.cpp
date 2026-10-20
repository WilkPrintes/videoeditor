#include "core/Commands.h"

#include <algorithm>
#include <cassert>

namespace ve {

bool Command::mergeWith(const Command&) const
{
    return false;
}

// ---------------------------------------------------------------------------------------------
// TimelineEditCommand
// ---------------------------------------------------------------------------------------------

TimelineEditCommand::TimelineEditCommand(std::string name, Operation op, std::string mergeKey)
    : m_name(std::move(name))
    , m_op(std::move(op))
    , m_mergeKey(std::move(mergeKey))
{
}

bool TimelineEditCommand::redo(Project& project)
{
    if (!m_executed) {
        m_executed = true;
        Timeline& tl = project.timeline();
        m_timelineId = tl.id;
        m_before = tl;
        // Atomic: a refused or partial operation leaves the timeline as it was.
        if (!m_op(tl, project.media) || tl == m_before) {
            tl = m_before;
            return false;
        }
        m_after = tl;
        m_op = nullptr; // the snapshots are all that is needed from now on
        return true;
    }
    *project.findTimeline(m_timelineId) = m_after;
    return true;
}

void TimelineEditCommand::undo(Project& project)
{
    *project.findTimeline(m_timelineId) = m_before;
}

bool TimelineEditCommand::mergeWith(const Command& next) const
{
    const auto* other = dynamic_cast<const TimelineEditCommand*>(&next);
    return other && !m_mergeKey.empty() && other->m_mergeKey == m_mergeKey;
}

// ---------------------------------------------------------------------------------------------
// ProjectEditCommand
// ---------------------------------------------------------------------------------------------

ProjectEditCommand::ProjectEditCommand(std::string name, Operation op, std::string mergeKey)
    : m_name(std::move(name))
    , m_op(std::move(op))
    , m_mergeKey(std::move(mergeKey))
{
}

ProjectEditCommand::State ProjectEditCommand::capture(const Project& project)
{
    return {project.timelines, project.currentTimeline};
}

void ProjectEditCommand::restore(Project& project, const State& state)
{
    project.timelines = state.timelines;
    project.currentTimeline = state.current;
}

bool ProjectEditCommand::redo(Project& project)
{
    if (!m_executed) {
        m_executed = true;
        m_before = capture(project);
        if (!m_op(project) || capture(project) == m_before) {
            restore(project, m_before);
            return false;
        }
        m_after = capture(project);
        m_op = nullptr;
        return true;
    }
    restore(project, m_after);
    return true;
}

void ProjectEditCommand::undo(Project& project)
{
    restore(project, m_before);
}

bool ProjectEditCommand::mergeWith(const Command& next) const
{
    const auto* other = dynamic_cast<const ProjectEditCommand*>(&next);
    return other && !m_mergeKey.empty() && other->m_mergeKey == m_mergeKey;
}

// ---------------------------------------------------------------------------------------------
// AddMediaCommand
// ---------------------------------------------------------------------------------------------

bool AddMediaCommand::redo(Project& project)
{
    if (project.media.find(m_media.id))
        return false;
    project.media.items.push_back(m_media);
    project.media.root.media.push_back(m_media.id);
    return true;
}

void AddMediaCommand::undo(Project& project)
{
    auto& items = project.media.items;
    items.erase(std::remove_if(items.begin(), items.end(), [&](const MediaItem& m) { return m.id == m_media.id; }),
                items.end());
    auto& ids = project.media.root.media;
    ids.erase(std::remove(ids.begin(), ids.end(), m_media.id), ids.end());
}

// ---------------------------------------------------------------------------------------------
// MacroCommand
// ---------------------------------------------------------------------------------------------

std::unique_ptr<MacroCommand> MacroCommand::fromExecuted(std::string name, std::vector<std::unique_ptr<Command>> done)
{
    auto macro = std::make_unique<MacroCommand>(std::move(name));
    macro->m_children = std::move(done);
    for (auto& child : macro->m_children)
        macro->m_applied.push_back(child.get());
    macro->m_executed = true;
    return macro;
}

bool MacroCommand::redo(Project& project)
{
    if (!m_executed) {
        m_executed = true;
        for (auto& child : m_children) {
            if (child->redo(project))
                m_applied.push_back(child.get());
        }
        return !m_applied.empty();
    }
    for (Command* child : m_applied)
        child->redo(project);
    return true;
}

void MacroCommand::undo(Project& project)
{
    for (auto it = m_applied.rbegin(); it != m_applied.rend(); ++it)
        (*it)->undo(project);
}

// ---------------------------------------------------------------------------------------------
// CommandStack
// ---------------------------------------------------------------------------------------------

bool CommandStack::push(std::unique_ptr<Command> cmd)
{
    if (!cmd)
        return false;
    if (!m_inTransaction) {
        if (!cmd->redo(m_project))
            return false;
        record(std::move(cmd));
        notify();
        return true;
    }

    // Inside a transaction: a newer step of the same gesture replaces the previous one.
    bool rolledBack = false;
    if (!m_transaction.empty() && m_transaction.back()->mergeWith(*cmd)) {
        m_transaction.back()->undo(m_project);
        m_transaction.pop_back();
        rolledBack = true;
    }
    const bool changed = cmd->redo(m_project);
    if (changed)
        m_transaction.push_back(std::move(cmd));
    if (changed || rolledBack)
        notify();
    return changed;
}

bool CommandStack::edit(std::string name, TimelineEditCommand::Operation op, std::string mergeKey)
{
    return push(std::make_unique<TimelineEditCommand>(std::move(name), std::move(op), std::move(mergeKey)));
}

bool CommandStack::editProject(std::string name, ProjectEditCommand::Operation op, std::string mergeKey)
{
    return push(std::make_unique<ProjectEditCommand>(std::move(name), std::move(op), std::move(mergeKey)));
}

void CommandStack::beginTransaction(std::string name)
{
    assert(!m_inTransaction && "transactions do not nest");
    if (m_inTransaction)
        abort();
    m_inTransaction = true;
    m_transactionName = std::move(name);
    m_transaction.clear();
}

bool CommandStack::commit()
{
    if (!m_inTransaction)
        return false;
    m_inTransaction = false;
    if (m_transaction.empty()) {
        notify();
        return false;
    }
    std::unique_ptr<Command> step;
    if (m_transaction.size() == 1) {
        step = std::move(m_transaction.front());
        m_transaction.clear();
    } else {
        step = MacroCommand::fromExecuted(m_transactionName, std::move(m_transaction));
        m_transaction.clear();
    }
    record(std::move(step));
    notify();
    return true;
}

void CommandStack::abort()
{
    if (!m_inTransaction)
        return;
    for (auto it = m_transaction.rbegin(); it != m_transaction.rend(); ++it)
        (*it)->undo(m_project);
    m_transaction.clear();
    m_inTransaction = false;
    notify();
}

void CommandStack::record(std::unique_ptr<Command> cmd)
{
    if (m_cleanIndex > std::ptrdiff_t(m_done.size()))
        m_cleanIndex = -1; // the saved state was in the redo history being dropped
    m_undone.clear();
    m_done.push_back(std::move(cmd));
}

std::string CommandStack::undoText() const
{
    return m_done.empty() ? std::string() : m_done.back()->name();
}

std::string CommandStack::redoText() const
{
    return m_undone.empty() ? std::string() : m_undone.back()->name();
}

void CommandStack::undo()
{
    if (m_inTransaction)
        abort();
    if (m_done.empty())
        return;
    std::unique_ptr<Command> cmd = std::move(m_done.back());
    m_done.pop_back();
    cmd->undo(m_project);
    m_undone.push_back(std::move(cmd));
    notify();
}

void CommandStack::redo()
{
    if (m_inTransaction || m_undone.empty())
        return;
    std::unique_ptr<Command> cmd = std::move(m_undone.back());
    m_undone.pop_back();
    cmd->redo(m_project);
    m_done.push_back(std::move(cmd));
    notify();
}

bool CommandStack::isClean() const
{
    return !m_inTransaction && m_cleanIndex == std::ptrdiff_t(m_done.size());
}

void CommandStack::setClean()
{
    m_cleanIndex = std::ptrdiff_t(m_done.size());
    notify();
}

void CommandStack::clear()
{
    m_inTransaction = false;
    m_transaction.clear();
    m_done.clear();
    m_undone.clear();
    m_cleanIndex = 0;
    notify();
}

void CommandStack::notify()
{
    if (onChanged)
        onChanged();
}

} // namespace ve
