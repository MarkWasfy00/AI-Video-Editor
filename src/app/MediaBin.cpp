#include "app/MediaBin.h"

#include "core/Project.h"

#include <QDragEnterEvent>
#include <QMimeData>
#include <QUrl>

namespace ve {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}

MediaBin::MediaBin(Project* project, QWidget* parent)
    : QListWidget(parent)
    , m_project(project)
{
    setViewMode(QListView::IconMode);
    setIconSize(QSize(128, 72));
    setGridSize(QSize(148, 112));
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setWordWrap(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);

    connect(m_project, &Project::mediaChanged, this, &MediaBin::rebuild);
    connect(this, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        emit mediaActivated(item->data(kIdRole).toString());
    });
    rebuild();
}

QString MediaBin::selectedMediaId() const
{
    const auto items = selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(kIdRole).toString();
}

void MediaBin::rebuild()
{
    const QString selected = selectedMediaId();
    clear();
    for (const auto& m : m_project->media()) {
        auto* item = new QListWidgetItem(this);
        item->setText(m.name);
        item->setData(kIdRole, m.id);
        if (!m.thumbnail.isNull()) item->setIcon(QIcon(QPixmap::fromImage(m.thumbnail)));
        QString tip = m.path;
        if (m.info.hasVideo) tip += QStringLiteral("\n%1x%2 %3").arg(m.info.width).arg(m.info.height).arg(m.info.videoCodec);
        if (m.info.hasVideo && !m.info.isImage) tip += QStringLiteral(" @ %1 fps").arg(m.info.fps, 0, 'f', 2);
        if (m.info.hasAudio) tip += QStringLiteral("\nAudio: %1 Hz %2 ch %3").arg(m.info.sampleRate).arg(m.info.channels).arg(m.info.audioCodec);
        if (!m.info.isImage) tip += QStringLiteral("\nDuration: %1 s").arg(usToSeconds(m.info.duration), 0, 'f', 2);
        item->setToolTip(tip);
        if (m.id == selected) item->setSelected(true);
    }
}

QStringList MediaBin::mimeTypes() const
{
    return {QString::fromLatin1(kMediaMimeType), QStringLiteral("text/uri-list")};
}

QMimeData* MediaBin::mimeData(const QList<QListWidgetItem*>& items) const
{
    if (items.isEmpty()) return nullptr;
    auto* md = new QMimeData;
    md->setData(QString::fromLatin1(kMediaMimeType), items.first()->data(kIdRole).toString().toUtf8());
    return md;
}

void MediaBin::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    else QListWidget::dragEnterEvent(e);
}

void MediaBin::dragMoveEvent(QDragMoveEvent* e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    else QListWidget::dragMoveEvent(e);
}

void MediaBin::dropEvent(QDropEvent* e)
{
    if (!e->mimeData()->hasUrls()) {
        e->ignore();
        return;
    }
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    if (!paths.isEmpty()) emit filesDropped(paths);
    e->acceptProposedAction();
}

} // namespace ve
