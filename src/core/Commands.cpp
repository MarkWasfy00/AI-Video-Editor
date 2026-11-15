#include "core/Commands.h"

#include "core/Project.h"

namespace ve {

namespace {
constexpr int kSnapshotCommandId = 0x5645; // 'VE'
}

SnapshotCommand::SnapshotCommand(Project* project, const QString& text, QJsonArray before,
                                 QJsonArray after, QString mergeKey)
    : m_project(project)
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_mergeKey(std::move(mergeKey))
{
    setText(text);
}

void SnapshotCommand::undo()
{
    m_project->restoreState(m_before);
}

void SnapshotCommand::redo()
{
    // The edit was already applied when the command was pushed.
    if (m_firstRedo) {
        m_firstRedo = false;
        return;
    }
    m_project->restoreState(m_after);
}

int SnapshotCommand::id() const
{
    return m_mergeKey.isEmpty() ? -1 : kSnapshotCommandId;
}

bool SnapshotCommand::mergeWith(const QUndoCommand* other)
{
    if (other->id() != id())
        return false;
    const auto* o = static_cast<const SnapshotCommand*>(other);
    if (o->m_mergeKey != m_mergeKey)
        return false;
    m_after = o->m_after;
    return true;
}

} // namespace ve
