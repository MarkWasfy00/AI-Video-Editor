#pragma once

#include <QListWidget>

namespace ve {

class Project;

inline constexpr const char* kMediaMimeType = "application/x-videoeditor-media-id";

// Grid of imported media with thumbnails. Items can be dragged onto the
// timeline; files dropped here get imported.
class MediaBin : public QListWidget {
    Q_OBJECT
public:
    explicit MediaBin(Project* project, QWidget* parent = nullptr);

    QString selectedMediaId() const;

signals:
    void mediaActivated(const QString& mediaId);
    void filesDropped(const QStringList& paths);

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    void rebuild();
    Project* m_project;
};

} // namespace ve
