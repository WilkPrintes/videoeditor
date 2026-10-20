#pragma once

// Commands, transactions and undo (spec §5.3).
//
// Every change to a Project goes through CommandStack::push. A transaction groups everything pushed
// between beginTransaction() and commit() into one undo step; abort() rolls it back. Interactive
// gestures (a trim drag, a slider) open a transaction on press, push a command per mouse move and
// commit on release. Consecutive commands of the same gesture *merge*: the newer one replaces the
// older, applied again from the state before the gesture, so intermediate positions never
// accumulate (dragging a clip across another and back leaves the other clip intact).
//
// Timeline edits are snapshot based: the edit function runs once and undo/redo swap the before/after
// copies of that timeline, so no edit needs hand-written inverse logic.

#include "core/Model.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ve {

class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;
    // Executes (first call) or re-applies the command. False = it changed nothing.
    virtual bool redo(Project& project) = 0;
    virtual void undo(Project& project) = 0;
    // True when `next` is a newer version of the same gesture; the stack then undoes this command
    // and runs `next` in its place.
    virtual bool mergeWith(const Command& next) const;
};

class TimelineEditCommand final : public Command {
public:
    using Operation = std::function<bool(Timeline&, const MediaPool&)>;

    // A non-empty mergeKey makes consecutive commands with the same key (on the same timeline)
    // merge inside a transaction.
    TimelineEditCommand(std::string name, Operation op, std::string mergeKey = {});

    std::string name() const override { return m_name; }
    bool redo(Project& project) override;
    void undo(Project& project) override;
    bool mergeWith(const Command& next) const override;

private:
    std::string m_name;
    Operation m_op;
    std::string m_mergeKey;
    Id m_timelineId;
    Timeline m_before;
    Timeline m_after;
    bool m_executed = false;
};

// Adds a media item to the pool (and to the root folder).
class AddMediaCommand final : public Command {
public:
    explicit AddMediaCommand(MediaItem media) : m_media(std::move(media)) {}

    std::string name() const override { return "Import " + m_media.name; }
    bool redo(Project& project) override;
    void undo(Project& project) override;

private:
    MediaItem m_media;
};

// Several commands that undo as one step.
class MacroCommand final : public Command {
public:
    explicit MacroCommand(std::string name) : m_name(std::move(name)) {}

    void add(std::unique_ptr<Command> cmd) { m_children.push_back(std::move(cmd)); }
    // For commands that already ran (a committed transaction).
    static std::unique_ptr<MacroCommand> fromExecuted(std::string name, std::vector<std::unique_ptr<Command>> done);

    std::string name() const override { return m_name; }
    bool redo(Project& project) override;
    void undo(Project& project) override;

private:
    std::string m_name;
    std::vector<std::unique_ptr<Command>> m_children;
    std::vector<Command*> m_applied;
    bool m_executed = false;
};

class CommandStack {
public:
    explicit CommandStack(Project& project) : m_project(project) {}

    // Executes the command. Outside a transaction it becomes one undo step; inside, it joins the
    // transaction (merging with the previous command when mergeWith() says so). False = no change.
    bool push(std::unique_ptr<Command> cmd);
    bool edit(std::string name, TimelineEditCommand::Operation op, std::string mergeKey = {});

    void beginTransaction(std::string name);
    bool commit(); // true if the transaction changed something (one new undo step)
    void abort();  // restores the state from before beginTransaction()
    bool inTransaction() const { return m_inTransaction; }

    bool canUndo() const { return !m_inTransaction && !m_done.empty(); }
    bool canRedo() const { return !m_inTransaction && !m_undone.empty(); }
    std::string undoText() const;
    std::string redoText() const;
    void undo();
    void redo();

    bool isClean() const;
    void setClean();
    void clear(); // forget history, e.g. after loading another project

    // Called after every change of the project or of the history.
    std::function<void()> onChanged;

private:
    void record(std::unique_ptr<Command> cmd);
    void notify();

    Project& m_project;
    std::vector<std::unique_ptr<Command>> m_done;
    std::vector<std::unique_ptr<Command>> m_undone;
    std::ptrdiff_t m_cleanIndex = 0; // m_done.size() at the last save; -1 = unreachable

    bool m_inTransaction = false;
    std::string m_transactionName;
    std::vector<std::unique_ptr<Command>> m_transaction; // executed commands of the open transaction
};

} // namespace ve
