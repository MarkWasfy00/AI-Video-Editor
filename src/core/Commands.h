#pragma once

#include <QJsonArray>
#include <QUndoCommand>

namespace ve {

class Project;

// Undo command that stores the full track state before/after an edit.
// Timelines of a basic editor are small, so this is simple and robust.
class SnapshotCommand : public QUndoCommand {
public:
    SnapshotCommand(Project* project, const QString& text, QJsonArray before, QJsonArray after,
                    QString mergeKey = {});

    void undo() override;
    void redo() override;
    int id() const override;
    bool mergeWith(const QUndoCommand* other) override;

private:
    Project* m_project;
    QJsonArray m_before;
    QJsonArray m_after;
    QString m_mergeKey;
    bool m_firstRedo = true;
};

} // namespace ve
