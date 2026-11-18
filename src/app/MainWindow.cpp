#include "app/MainWindow.h"

#include "api/McpServer.h"
#include "api/McpTransports.h"
#include "app/AiModelsDialog.h"
#include "app/ClaudePanel.h"
#include "app/UpscaleDialog.h"
#include "app/Theme.h"
#include "app/TranscriptPanel.h"
#include "app/CaptionPanel.h"
#include "app/SpeechDialog.h"
#include "app/TextPanel.h"
#include "app/PreviewInteractor.h"
#include "media/Speech.h"
#include "app/ExportDialog.h"
#include "app/GpuSettingsDialog.h"
#include "app/Inspector.h"
#include "app/MediaBin.h"
#include "app/MixerPanel.h"
#include "app/PlaybackController.h"
#include "app/TimelineWidget.h"
#include "core/Project.h"
#include "core/ProjectStore.h"
#include "media/Exporter.h"
#include "media/FFmpegUtil.h"
#include "media/HwAccel.h"
#include "media/MediaProbe.h"
#include "media/StemSeparator.h"
#include "media/VideoUpscaler.h"
#include "media/Waveform.h"
#include "render/GLPreviewWidget.h"
#include "render/PreviewEngine.h"

#ifdef VE_HAVE_VULKAN
#include "render/VulkanPreview.h"
#endif

#include <QActionGroup>
#include <QClipboard>
#include <QGuiApplication>
#include <QTextBrowser>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

#include <sqlite3.h>

#include <cmath>

namespace ve {

namespace {
// Bump when the default dock layout changes, so older saved layouts are ignored.
constexpr int kWindowStateVersion = 6; // bumped: old saved layouts had the side panels split apart
const QString kProjectFilter = QObject::tr("Video Editor Project (*.veproj);;JSON Project (*.json)");
const QString kMediaFilter = QObject::tr(
    "Media (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mpg *.mpeg *.ts *.mts *.flv *.wmv "
    "*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.opus *.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff);;All files (*)");
constexpr int kMaxRecent = 8;
}

MainWindow::MainWindow(RenderBackend backend, int mcpPort, QWidget* parent)
    : QMainWindow(parent)
{
    m_project = new Project(this);
    m_engine = new PreviewEngine(this);
    m_playback = new PlaybackController(m_project, m_engine, this);
    m_api = std::make_unique<EditorApi>(m_project, this);
    m_mcp = new McpServer(m_api.get(), this);
    m_mcpHttp = new McpHttpTransport(m_mcp, this);

    createLayout();
    createActions();
    // Click / drag / resize / rotate pictures and text right in the preview.
    m_interactor = new PreviewInteractor(
        m_project,
        {[this] { return m_timeline->selectedClip(); }, [this](const QString& id) { m_timeline->setSelectedClip(id); },
         [this](const QString& id) {
             m_timeline->setSelectedClip(id);
             m_textDock->show();
             m_textDock->raise();
             m_textPanel->focusText();
         },
         [this] { return m_playback->isPlaying(); }, [this] { return m_lastFrame; }},
        this);
    setRenderer(backend);

    connect(m_engine, &PreviewEngine::frameReady, this, [this](CompositeFramePtr f) {
        m_lastFrame = f;
        updateDecodeStatus(f);
        if (m_preview) m_preview->setFrame(m_interactor->decorate(f));
    });
    const auto redecorate = [this] { if (m_preview) m_preview->setFrame(m_interactor->decorate(m_lastFrame)); };
    connect(m_timeline, &TimelineWidget::selectionChanged, this, redecorate);
    connect(m_playback, &PlaybackController::playingChanged, this, redecorate);
    connect(m_playback, &PlaybackController::positionChanged, m_timeline, &TimelineWidget::setPlayhead);
    connect(m_playback, &PlaybackController::positionChanged, this, &MainWindow::updateTimecode);
    connect(m_playback, &PlaybackController::playingChanged, this, [this](bool playing) {
        m_actPlay->setIcon(theme::icon(playing ? theme::Glyph::Pause : theme::Glyph::Play));
        m_actPlay->setText(playing ? tr("Pause") : tr("Play"));
    });
    connect(m_timeline, &TimelineWidget::seekRequested, m_playback, &PlaybackController::seek);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_inspector, &Inspector::setClip);
    connect(m_timeline, &TimelineWidget::filesDropped, this,
            [this](const QStringList& paths, int track, TimeUs at) { importFiles(paths, track, at); });
    connect(m_mediaBin, &MediaBin::filesDropped, this, [this](const QStringList& p) { importFiles(p); });
    connect(m_mediaBin, &MediaBin::mediaActivated, this, &MainWindow::addMediaAtPlayhead);
    connect(m_project, &Project::modifiedChanged, this, &MainWindow::updateTitle);
    connect(m_project, &Project::changed, this, &MainWindow::updateTimecode);
    connect(m_project, &Project::settingsChanged, this, &MainWindow::updateStatus);
    connect(m_project, &Project::mediaChanged, this, &MainWindow::ensureWaveforms);
    connect(m_project, &Project::changed, this, &MainWindow::updateLinkActions);
    connect(m_timeline, &TimelineWidget::separateAudioRequested, this, &MainWindow::separateAudio);
    connect(m_timeline, &TimelineWidget::transcribeRequested, this, [this](const QString& id) { transcribeClip(id); });
    connect(m_timeline, &TimelineWidget::autoCaptionsRequested, this, [this](const QString& id) {
        m_timeline->setSelectedClip(id);
        autoCaptions();
    });
    connect(m_playback, &PlaybackController::positionChanged, m_transcript, &TranscriptPanel::setPlayhead);
    connect(m_timeline, &TimelineWidget::selectionChanged, this, &MainWindow::updateLinkActions);
    connect(m_timeline, &TimelineWidget::selectionChanged, m_textPanel, &TextPanel::setClip);

    // Mixer meters (~30 Hz), fed with the audio actually played up to the playhead.
    auto* meterTimer = new QTimer(this);
    meterTimer->setInterval(33);
    connect(meterTimer, &QTimer::timeout, this, [this] {
        MixLevels lv;
        const bool got = m_playback->takeAudioLevels(lv);
        if (got) m_lastLevels = lv;
        else if (!m_playback->isPlaying()) m_lastLevels.clear();
        if (m_mixerDock->isVisible()) m_mixer->updateMeters(got ? &lv : nullptr);
    });
    meterTimer->start();

    // MCP endpoint for Claude (Agent SDK / Claude Code / Desktop)
    connect(m_mcp, &McpServer::toolCalled, this, [this](const QString& name, bool ok) {
        statusBar()->showMessage(tr("Claude (MCP): %1 %2").arg(name, ok ? tr("done") : tr("failed")), 4000);
    });
    {
        QSettings s;
        m_mcpPort = quint16(mcpPort > 0 ? mcpPort : s.value(QStringLiteral("mcp/port"), 8765).toInt());
        const bool enabled = mcpPort >= 0 && s.value(QStringLiteral("mcp/enabled"), true).toBool();
        const QSignalBlocker block(m_actMcp);
        m_actMcp->setChecked(enabled);
        setMcpEnabled(enabled);
    }

    resize(1500, 920);
    QSettings settings;
    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    if (!restoreState(settings.value(QStringLiteral("window/state")).toByteArray(), kWindowStateVersion))
        QTimer::singleShot(0, this, &MainWindow::resetPanelLayout); // sizes need the shown window
    updateTitle();
    updateTimecode();
    updateStatus();
    updateLinkActions();

    // Limited hardware: offer the AI models that fit this computer (once per hardware).
    QTimer::singleShot(900, this, [this] { AiModelsDialog::promptIfLimited(this); });
}

void MainWindow::resetPanelLayout()
{
    for (QDockWidget* d : {m_mediaDock, m_timelineDock}) {
        d->setFloating(false);
        d->show();
    }
    addDockWidget(Qt::LeftDockWidgetArea, m_mediaDock);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
    arrangeInspectorAndMixer(PanelLayout::SideBySide);
    // Side panels + preview get the top half, the timeline the bottom half.
    const int h = centralWidget()->height() + m_timelineDock->height();
    resizeDocks({m_timelineDock}, {h / 2}, Qt::Vertical);
    resizeDocks({m_mediaDock}, {300}, Qt::Horizontal);
}

void MainWindow::arrangeInspectorAndMixer(PanelLayout layout)
{
    for (QDockWidget* d : {m_inspectorDock, m_mixerDock, m_claudeDock}) d->setFloating(false);
    if (m_transcriptDock) removeDockWidget(m_transcriptDock);
    if (m_captionDock) removeDockWidget(m_captionDock);
    Qt::DockWidgetArea area = dockWidgetArea(m_inspectorDock);
    if (area == Qt::NoDockWidgetArea) area = Qt::RightDockWidgetArea;

    // Rebuild from scratch: Qt keeps a dock marked as tabbed even after its tab
    // partner is removed, and splitting next to a tabbed dock only adds a tab.
    for (QDockWidget* d : {m_inspectorDock, m_mixerDock, m_claudeDock}) removeDockWidget(d);
    addDockWidget(area, m_mixerDock);
    addDockWidget(area, m_inspectorDock);
    switch (layout) {
    case PanelLayout::SideBySide:   // Mixer | Inspector + Claude (top-right corner)
        splitDockWidget(m_mixerDock, m_inspectorDock, Qt::Horizontal);
        break;
    case PanelLayout::Stacked:
        splitDockWidget(m_inspectorDock, m_mixerDock, Qt::Vertical);
        break;
    case PanelLayout::Tabbed:
        tabifyDockWidget(m_inspectorDock, m_mixerDock);
        break;
    }
    // Claude and the Transcript share the Inspector's spot (top right) as tabs.
    addDockWidget(area, m_claudeDock);
    tabifyDockWidget(m_inspectorDock, m_claudeDock);
    if (m_transcriptDock) {
        m_transcriptDock->setFloating(false);
        removeDockWidget(m_transcriptDock);
        addDockWidget(area, m_transcriptDock);
        tabifyDockWidget(m_inspectorDock, m_transcriptDock);
        m_transcriptDock->show();
    }
    if (m_captionDock) {
        m_captionDock->setFloating(false);
        removeDockWidget(m_captionDock);
        addDockWidget(area, m_captionDock);
        tabifyDockWidget(m_inspectorDock, m_captionDock);
        m_captionDock->show();
    }
    if (m_textDock) {
        m_textDock->setFloating(false);
        removeDockWidget(m_textDock);
        addDockWidget(area, m_textDock);
        tabifyDockWidget(m_inspectorDock, m_textDock);
        m_textDock->show();
    }
    for (QDockWidget* d : {m_inspectorDock, m_mixerDock, m_claudeDock}) d->show();

    if (layout == PanelLayout::SideBySide)
        resizeDocks({m_mixerDock, m_inspectorDock}, {330, 380}, Qt::Horizontal);
    else if (layout == PanelLayout::Stacked)
        resizeDocks({m_inspectorDock, m_mixerDock}, {height() / 2, height() / 2}, Qt::Vertical);
    m_claudeDock->raise();
}

void MainWindow::showClaude()
{
    m_claudeDock->show();
    m_claudeDock->raise();
    m_claude->focusInput();
}

QString MainWindow::claudeMcpUrl()
{
    if (!m_mcpHttp->isListening() && !m_actMcp->isChecked()) m_actMcp->setChecked(true); // starts the server
    if (!m_mcpHttp->isListening()) m_mcpHttp->listen(0); // configured port busy: use any free local port
    return m_mcpHttp->isListening() ? m_mcpHttp->url() : QString();
}

QString MainWindow::editorStateForClaude() const
{
    const double fps = m_project->settings().fps;
    QStringList parts;
    parts << tr("playhead %1 s of %2 s (%3)")
                 .arg(usToSeconds(m_playback->position()), 0, 'f', 2)
                 .arg(usToSeconds(m_project->duration()), 0, 'f', 2)
                 .arg(formatTimecode(m_playback->position(), fps));
    const QString sel = m_timeline->selectedClip();
    int track = -1;
    if (const Clip* c = sel.isEmpty() ? nullptr : m_project->findClip(sel, &track)) {
        parts << tr("selected clip \"%1\" id %2 on track %3 (%4), %5-%6 s")
                     .arg(c->name, c->id).arg(track).arg(m_project->tracks()[track].name)
                     .arg(usToSeconds(c->start), 0, 'f', 2).arg(usToSeconds(c->end()), 0, 'f', 2);
    } else {
        parts << tr("no clip selected");
    }
    QStringList tracks;
    for (int i = 0; i < m_project->trackCount(); ++i) tracks << QStringLiteral("%1=%2").arg(i).arg(m_project->tracks()[i].name);
    parts << tr("tracks %1").arg(tracks.join(QStringLiteral(", ")));
    if (!m_project->filePath().isEmpty()) parts << tr("project file %1").arg(m_project->filePath());
    return parts.join(QStringLiteral("; "));
}

bool MainWindow::audioLevels(MixLevels& out) const
{
    if (!m_playback->isPlaying() || m_lastLevels.isEmpty()) return false;
    out = m_lastLevels;
    return true;
}

void MainWindow::ensureWaveforms()
{
    for (const MediaItem& m : m_project->media()) {
        if (!m.info.hasAudio || m.info.isImage || m.waveform || m.info.duration <= 0) continue;
        if (const auto it = m_waveCache.constFind(m.path); it != m_waveCache.constEnd()) {
            const QString id = m.id;
            const auto peaks = it.value();
            QTimer::singleShot(0, this, [this, id, peaks] { m_project->setMediaWaveform(id, peaks); });
            continue;
        }
        if (m_wavePending.contains(m.path)) continue;
        m_wavePending.insert(m.path);
        QPointer<MainWindow> self(this);
        const QString path = m.path;
        const TimeUs duration = m.info.duration;
        const auto cancel = m_waveCancel;
        QThreadPool::globalInstance()->start([self, path, duration, cancel] {
            auto peaks = computeWaveform(path, duration, cancel.get());
            QMetaObject::invokeMethod(qApp, [self, path, peaks] {
                if (!self) return;
                self->m_wavePending.remove(path);
                if (!peaks) return;
                self->m_waveCache.insert(path, peaks);
                for (const MediaItem& x : self->m_project->media())
                    if (x.path == path && !x.waveform) self->m_project->setMediaWaveform(x.id, peaks);
            }, Qt::QueuedConnection);
        });
    }
}

MainWindow::~MainWindow()
{
    m_waveCancel->store(true);
    m_preview.reset();
}

// ---------------------------------------------------------------------------
// Layout + actions
// ---------------------------------------------------------------------------
void MainWindow::createLayout()
{
    // Preview + transport
    auto* previewPanel = new QWidget(this);
    m_previewLayout = new QVBoxLayout(previewPanel);
    m_previewLayout->setContentsMargins(0, 0, 0, 0);
    m_previewLayout->setSpacing(2);

    auto* transport = new QWidget(previewPanel);
    auto* th = new QHBoxLayout(transport);
    th->setContentsMargins(10, 5, 10, 5);
    th->setSpacing(2);
    auto makeButton = [&](theme::Glyph icon, const QString& tip, auto slot) {
        auto* b = new QToolButton(transport);
        b->setIcon(theme::icon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setIconSize(QSize(20, 20));
        connect(b, &QToolButton::clicked, this, slot);
        th->addWidget(b);
        return b;
    };
    makeButton(theme::Glyph::SkipBack, tr("Go to start (Home)"), [this] { m_playback->seek(0); });
    makeButton(theme::Glyph::StepBack, tr("Previous frame (Left)"), [this] { m_playback->stepFrames(-1); });
    m_playButton = makeButton(theme::Glyph::Play, tr("Play / Pause (Space)"), [this] { m_playback->togglePlay(); });
    makeButton(theme::Glyph::StepForward, tr("Next frame (Right)"), [this] { m_playback->stepFrames(1); });
    makeButton(theme::Glyph::SkipForward, tr("Go to end (End)"), [this] { m_playback->goToEnd(); });
    th->addSpacing(12);
    m_timecode = new QLabel(transport);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(11);
    m_timecode->setFont(mono);
    th->addWidget(m_timecode);
    th->addStretch(1);

    // Linked selection: when off, clips are edited on their own (audio separate from video).
    m_actLinkedSelection = new QAction(QStringLiteral("\u26AD ") + tr("Linked"), this);
    m_actLinkedSelection->setCheckable(true);
    m_actLinkedSelection->setChecked(QSettings().value(QStringLiteral("timeline/linkedSelection"), true).toBool());
    m_actLinkedSelection->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    m_actLinkedSelection->setToolTip(tr("Linked selection (Ctrl+Shift+L): when on, a video and its audio move, trim, "
                                        "split and delete together. Turn off to edit audio and video separately."));
    m_actLinkOnAdd = new QAction(tr("Link Audio to Video When &Adding Clips"), this);
    m_actLinkOnAdd->setCheckable(true);
    m_actLinkOnAdd->setChecked(QSettings().value(QStringLiteral("timeline/linkOnAdd"), true).toBool());
    m_actLinkOnAdd->setToolTip(tr("When off, a video's sound is placed on its audio track as an independent clip"));
    auto* linkedBtn = new QToolButton(transport);
    linkedBtn->setDefaultAction(m_actLinkedSelection);
    linkedBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    linkedBtn->setStyleSheet(QStringLiteral("QToolButton { padding: 4px 10px; }"));
    th->addWidget(linkedBtn);
    m_previewLayout->addWidget(transport);

    m_timeline = new TimelineWidget(m_project, this);
    m_timeline->setLinkedSelection(m_actLinkedSelection->isChecked());
    m_timeline->setLinkOnAdd(m_actLinkOnAdd->isChecked());
    connect(m_actLinkedSelection, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("timeline/linkedSelection"), on);
        m_timeline->setLinkedSelection(on);
        statusBar()->showMessage(on ? tr("Linked selection on: audio and video are edited together")
                                    : tr("Linked selection off: audio and video are edited separately"), 4000);
    });
    connect(m_actLinkOnAdd, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("timeline/linkOnAdd"), on);
        m_timeline->setLinkOnAdd(on);
    });

    setCentralWidget(previewPanel);

    // The timeline is a bottom panel that owns both bottom corners: it runs the
    // full width of the window, the side panels (Media, Inspector, Audio Mixer)
    // fill the top half, and the preview takes the space between them.
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    m_timelineDock = new QDockWidget(tr("Timeline"), this);
    m_timelineDock->setObjectName(QStringLiteral("TimelineDock"));
    m_timelineDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    m_timelineDock->setWidget(m_timeline);
    addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);

    // Docks
    m_mediaBin = new MediaBin(m_project, this);
    auto* mediaDock = new QDockWidget(tr("Media"), this);
    m_mediaDock = mediaDock;
    mediaDock->setObjectName(QStringLiteral("MediaDock"));
    auto* mediaPanel = new QWidget(mediaDock);
    auto* mv = new QVBoxLayout(mediaPanel);
    mv->setContentsMargins(4, 4, 4, 4);
    auto* mediaButtons = new QHBoxLayout;
    auto* importBtn = new QPushButton(tr("Import..."), mediaPanel);
    auto* addBtn = new QPushButton(tr("Add to Timeline"), mediaPanel);
    auto* removeBtn = new QPushButton(tr("Remove"), mediaPanel);
    connect(importBtn, &QPushButton::clicked, this, &MainWindow::importDialog);
    connect(addBtn, &QPushButton::clicked, this, [this] {
        const QString id = m_mediaBin->selectedMediaId();
        if (!id.isEmpty()) addMediaAtPlayhead(id);
    });
    connect(removeBtn, &QPushButton::clicked, this, &MainWindow::removeSelectedMedia);
    mediaButtons->addWidget(importBtn);
    mediaButtons->addWidget(addBtn);
    mediaButtons->addWidget(removeBtn);
    mv->addLayout(mediaButtons);
    mv->addWidget(m_mediaBin, 1);
    mediaDock->setWidget(mediaPanel);
    addDockWidget(Qt::LeftDockWidgetArea, mediaDock);

    m_inspector = new Inspector(m_project, this);
    // Docks can be dragged anywhere, placed side by side or stacked inside an
    // area, tabbed, or floated. Dragging a group's title bar moves the whole tab
    // group together; dragging a single tab pulls that one panel out.
    setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);

    auto* inspectorDock = new QDockWidget(tr("Inspector"), this);
    m_inspectorDock = inspectorDock;
    inspectorDock->setObjectName(QStringLiteral("InspectorDock"));
    inspectorDock->setWidget(m_inspector);
    inspectorDock->setMinimumWidth(300);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock);

    m_mixer = new MixerPanel(m_project, this);
    m_mixerDock = new QDockWidget(tr("Audio Mixer"), this);
    m_mixerDock->setObjectName(QStringLiteral("MixerDock"));
    m_mixerDock->setWidget(m_mixer);
    addDockWidget(Qt::RightDockWidgetArea, m_mixerDock);

    // Claude: chat that edits the project through the editor's MCP tools.
    m_claude = new ClaudePanel({[this] { return claudeMcpUrl(); }, [this] { return editorStateForClaude(); }}, this);
    m_claudeDock = new QDockWidget(tr("Claude"), this);
    m_claudeDock->setObjectName(QStringLiteral("ClaudeDock"));
    m_claudeDock->setWidget(m_claude);
    addDockWidget(Qt::RightDockWidgetArea, m_claudeDock);

    // Transcript / lyrics
    m_transcript = new TranscriptPanel(
        m_project,
        {[this](TimeUs t) { m_playback->seek(t); }, [this] { return m_timeline->selectedClip(); },
         [this](const QString& id, const QString& lang, bool isolate, const QString& hints) {
             transcribeClip(id, lang, isolate, hints);
         },
         [this](const QString& fmt) { exportTranscript(fmt); }, [this] { importSubtitles(); },
         [this] { translateSubtitles(); }, [this] { voiceOverSubtitles(); }},
        this);
    m_transcriptDock = new QDockWidget(tr("Transcript"), this);
    m_transcriptDock->setObjectName(QStringLiteral("TranscriptDock"));
    m_transcriptDock->setWidget(m_transcript);
    addDockWidget(Qt::RightDockWidgetArea, m_transcriptDock);

    // Captions (CapCut-style text over the video, from the transcript)
    m_captionPanel = new CaptionPanel(m_project, this);
    m_captionDock = new QDockWidget(tr("Captions"), this);
    m_captionDock->setObjectName(QStringLiteral("CaptionDock"));
    m_captionDock->setWidget(m_captionPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_captionDock);

    // Text overlays (titles, lower thirds, ...)
    m_textPanel = new TextPanel(m_project, {[this](const QString& tpl) { addText(tpl); },
                                            [this](const QString& clip) { speakText(clip); }}, this);
    m_textDock = new QDockWidget(tr("Text"), this);
    m_textDock->setObjectName(QStringLiteral("TextDock"));
    m_textDock->setWidget(m_textPanel);
    addDockWidget(Qt::RightDockWidgetArea, m_textDock);

    arrangeInspectorAndMixer(PanelLayout::SideBySide); // default; a saved layout overrides it

    m_statusRenderer = new QLabel(this);
    m_statusProject = new QLabel(this);
    m_statusDecode = new QLabel(this);
    m_statusMcp = new QLabel(this);
    m_statusMcp->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusBar()->addWidget(m_statusRenderer, 1);
    statusBar()->addPermanentWidget(m_statusDecode);
    statusBar()->addPermanentWidget(m_statusMcp);
    statusBar()->addPermanentWidget(m_statusProject);
}

void MainWindow::createActions()
{
    // File
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New Project"), QKeySequence::New, this, &MainWindow::newProject);
    file->addAction(tr("&Open Project..."), QKeySequence::Open, this, &MainWindow::openDialog);
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    rebuildRecentMenu();
    file->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::save);
    file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveAs);
    file->addSeparator();
    file->addAction(tr("&Import Media..."), QKeySequence(Qt::CTRL | Qt::Key_I), this, &MainWindow::importDialog);
    file->addAction(tr("&Export Video..."), QKeySequence(Qt::CTRL | Qt::Key_E), this, &MainWindow::exportVideo);
    file->addSeparator();
    file->addAction(tr("Project &Settings..."), this, &MainWindow::projectSettingsDialog);
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // Edit
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undo = m_project->undoStack()->createUndoAction(this, tr("&Undo"));
    undo->setShortcut(QKeySequence::Undo);
    QAction* redo = m_project->undoStack()->createRedoAction(this, tr("&Redo"));
    redo->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::Key_Y)});
    edit->addAction(undo);
    edit->addAction(redo);
    edit->addSeparator();
    edit->addAction(tr("&Split at Playhead"), QKeySequence(Qt::Key_S), this, &MainWindow::splitAtPlayhead);
    edit->addAction(tr("&Delete Clip"), QKeySequence::Delete, this, &MainWindow::deleteSelected);
    edit->addAction(tr("Ripple Delete (Close Gap)"), QKeySequence(Qt::SHIFT | Qt::Key_Delete), this,
                    &MainWindow::rippleDeleteSelected);
    edit->addAction(tr("&Duplicate Clip"), QKeySequence(Qt::CTRL | Qt::Key_D), this, &MainWindow::duplicateSelected);
    edit->addSeparator();
    edit->addAction(tr("Mark &In"), QKeySequence(Qt::Key_I), this, [this] { m_timeline->setMarkIn(m_playback->position()); });
    edit->addAction(tr("Mark &Out"), QKeySequence(Qt::Key_O), this, [this] { m_timeline->setMarkOut(m_playback->position()); });
    edit->addAction(tr("Clear Marks"), QKeySequence(Qt::ALT | Qt::Key_X), this, [this] { m_timeline->clearMarks(); });
    edit->addAction(tr("&Cut Marked Range (Close Gap)"), QKeySequence(Qt::CTRL | Qt::Key_X), this, [this] { cutRange(true); });
    edit->addAction(tr("Delete Marked Range (Leave Gap)"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X), this,
                    [this] { cutRange(false); });
    edit->addAction(tr("Close &Gaps at Playhead"), QKeySequence(Qt::CTRL | Qt::Key_G), this, &MainWindow::closeGapsAtPlayhead);
    edit->addSeparator();
    m_actLinkToggle = edit->addAction(tr("&Unlink Audio and Video"), QKeySequence(Qt::CTRL | Qt::Key_L), this,
                                      &MainWindow::toggleLinkSelected);
    edit->addAction(m_actLinkedSelection);
    edit->addAction(m_actLinkOnAdd);
    m_actLinkedSelection->setText(tr("&Linked Selection"));
    edit->addSeparator();
    edit->addAction(tr("Add &Track"), this, [this] {
        m_project->edit(tr("Add track"), [this] { m_project->addTrack(); });
    });
    edit->addSeparator();
    edit->addAction(tr("&GPU && Hardware Acceleration..."), this, &MainWindow::gpuSettingsDialog);

    // Clip
    QMenu* clipMenu = menuBar()->addMenu(tr("&Clip"));
    clipMenu->addAction(tr("&Split at Playhead"), this, &MainWindow::splitAtPlayhead);
    clipMenu->addAction(tr("&Duplicate"), this, &MainWindow::duplicateSelected);
    clipMenu->addAction(tr("&Freeze Frame at Playhead"), QKeySequence(Qt::SHIFT | Qt::Key_F), this,
                        &MainWindow::freezeFrameSelected);
    clipMenu->addAction(tr("&Reverse"), QKeySequence(Qt::CTRL | Qt::Key_R), this, &MainWindow::reverseSelected);
    QMenu* speedMenu = clipMenu->addMenu(tr("S&peed"));
    for (double s : {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0, 8.0})
        speedMenu->addAction(QStringLiteral("%1x").arg(s), this, [this, s] { setSelectedSpeed(s); });
    speedMenu->addSeparator();
    speedMenu->addAction(tr("Custom Speed..."), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R), this,
                         &MainWindow::customSpeedDialog);
    QMenu* curveMenu = clipMenu->addMenu(tr("Speed &Curve"));
    const QList<QPair<QString, QString>> curves = {
        {QStringLiteral("none"), tr("None (constant)")}, {QStringLiteral("ease-in"), tr("Ease In (speed up)")},
        {QStringLiteral("ease-out"), tr("Ease Out (slow down)")}, {QStringLiteral("ease-in-out"), tr("Slow - Fast - Slow")},
        {QStringLiteral("bullet"), tr("Bullet (fast - slow-mo - fast)")}, {QStringLiteral("hero"), tr("Hero (slow-motion moment)")}};
    for (const auto& [key, label] : curves)
        curveMenu->addAction(label, this, [this, key] { setSelectedCurve(key); });
    clipMenu->addSeparator();
    clipMenu->addAction(tr("Separate &Voice and Music..."), this, [this] {
        QString id;
        if (requireSelection(&id)) separateAudio(id);
    });
    clipMenu->addAction(tr("&Transcribe Speech / Lyrics..."), this, [this] {
        QString id;
        if (requireSelection(&id)) transcribeClip(id);
    });
    clipMenu->addAction(tr("&Auto Captions"), this, &MainWindow::autoCaptions);
    clipMenu->addAction(tr("Import Su&btitles..."), this, &MainWindow::importSubtitles);
    clipMenu->addAction(tr("Trans&late Subtitles..."), this, &MainWindow::translateSubtitles);
    clipMenu->addAction(tr("&Upscale Video (AI)..."), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_U), this, [this] {
        QString id;
        if (requireSelection(&id)) upscaleClip(id);
    });
    clipMenu->addSeparator();
    clipMenu->addAction(tr("Add &Text"), QKeySequence(Qt::CTRL | Qt::Key_T), this, [this] {
        addText(m_textPanel ? m_textPanel->currentTemplate() : QStringLiteral("title"));
    });
    clipMenu->addAction(tr("Text to S&peech..."), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_T), this, [this] {
        const QString sel = m_timeline->selectedClip();
        const Clip* c = m_project->findClip(sel);
        speakText(c && c->isText() ? sel : QString());
    });
    clipMenu->addAction(tr("&Voice-over from Subtitles..."), this, &MainWindow::voiceOverSubtitles);
    clipMenu->addSeparator();
    clipMenu->addAction(tr("Fit to Frame"), this, [this] { fitSelected(false); });
    clipMenu->addAction(tr("Fill Frame"), this, [this] { fitSelected(true); });
    clipMenu->addAction(tr("Rotate 90\u00B0 Clockwise"), QKeySequence(Qt::CTRL | Qt::Key_BracketRight), this,
                        [this] { rotateSelected(90); });
    clipMenu->addAction(tr("Rotate 90\u00B0 Counter-clockwise"), QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), this,
                        [this] { rotateSelected(-90); });
    clipMenu->addAction(tr("Crop..."), this, [this] {
        m_inspectorDock->show();
        m_inspectorDock->raise();
        statusBar()->showMessage(tr("Use the Crop section in the Inspector"), 4000);
    });
    clipMenu->addAction(tr("Resize Canvas / Aspect Ratio..."), this, &MainWindow::projectSettingsDialog);

    // Playback
    QMenu* play = menuBar()->addMenu(tr("&Playback"));
    m_actPlay = play->addAction(theme::icon(theme::Glyph::Play), tr("Play"), QKeySequence(Qt::Key_Space),
                                m_playback, &PlaybackController::togglePlay);
    connect(m_actPlay, &QAction::changed, this, [this] { m_playButton->setIcon(m_actPlay->icon()); });
    play->addAction(tr("Previous Frame"), QKeySequence(Qt::Key_Left), this, [this] { m_playback->stepFrames(-1); });
    play->addAction(tr("Next Frame"), QKeySequence(Qt::Key_Right), this, [this] { m_playback->stepFrames(1); });
    play->addAction(tr("Back 1 Second"), QKeySequence(Qt::SHIFT | Qt::Key_Left), this,
                    [this] { m_playback->stepFrames(-int(std::lround(m_project->settings().fps))); });
    play->addAction(tr("Forward 1 Second"), QKeySequence(Qt::SHIFT | Qt::Key_Right), this,
                    [this] { m_playback->stepFrames(int(std::lround(m_project->settings().fps))); });
    play->addAction(tr("Go to Start"), QKeySequence(Qt::Key_Home), this, [this] { m_playback->seek(0); });
    play->addAction(tr("Go to End"), QKeySequence(Qt::Key_End), m_playback, &PlaybackController::goToEnd);

    // View
    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, m_timeline, &TimelineWidget::zoomIn);
    view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, m_timeline, &TimelineWidget::zoomOut);
    view->addAction(tr("Zoom to &Fit"), QKeySequence(Qt::CTRL | Qt::Key_0), m_timeline, &TimelineWidget::zoomToFit);
    view->addSeparator();
    QAction* mixerAct = view->addAction(tr("Audio &Mixer"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M), this, [this] {
        m_mixerDock->show();
        m_mixerDock->raise();
    });
    mixerAct->setToolTip(tr("Show the audio mixer"));
    QMenu* panels = view->addMenu(tr("Inspector && Mixer &Layout"));
    panels->addAction(tr("&Side by Side"), this, [this] { arrangeInspectorAndMixer(PanelLayout::SideBySide); });
    panels->addAction(tr("Stac&ked (Mixer Below)"), this, [this] { arrangeInspectorAndMixer(PanelLayout::Stacked); });
    panels->addAction(tr("&Tabbed"), this, [this] { arrangeInspectorAndMixer(PanelLayout::Tabbed); });
    panels->addSeparator();
    panels->addAction(tr("&Reset All Panels"), this, &MainWindow::resetPanelLayout);
    view->addAction(tr("&Claude Assistant"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C), this, &MainWindow::showClaude);
    view->addAction(tr("&Transcript / Lyrics"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T), this, [this] {
        m_transcriptDock->show();
        m_transcriptDock->raise();
    });
    view->addAction(tr("Caption &Style"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K), this, [this] {
        m_captionDock->show();
        m_captionDock->raise();
    });
    view->addAction(tr("Te&xt"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E), this, [this] {
        m_textDock->show();
        m_textDock->raise();
    });
    panels->addAction(tr("&Float Audio Mixer"), this, [this] {
        m_mixerDock->show();
        m_mixerDock->setFloating(true);
    });
    view->addSeparator();
    QMenu* renderer = view->addMenu(tr("Preview &Renderer"));
    auto* group = new QActionGroup(this);
    m_actOpenGL = renderer->addAction(tr("OpenGL"), this, [this] { setRenderer(RenderBackend::OpenGL); });
    m_actVulkan = renderer->addAction(tr("Vulkan"), this, [this] { setRenderer(RenderBackend::Vulkan); });
    for (QAction* a : {m_actOpenGL, m_actVulkan}) {
        a->setCheckable(true);
        group->addAction(a);
    }
#ifndef VE_HAVE_VULKAN
    m_actVulkan->setEnabled(false);
    m_actVulkan->setText(tr("Vulkan (not built)"));
#endif

    // Claude / MCP
    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
    m_actMcp = tools->addAction(tr("Enable &MCP Server for Claude"));
    m_actMcp->setCheckable(true);
    connect(m_actMcp, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("mcp/enabled"), on);
        setMcpEnabled(on);
    });
    tools->addAction(tr("&Copy MCP Endpoint URL"), this, [this] {
        QGuiApplication::clipboard()->setText(m_mcpHttp->url());
        statusBar()->showMessage(tr("Copied %1").arg(m_mcpHttp->url()), 3000);
    });
    tools->addAction(tr("MCP &Setup Help..."), this, &MainWindow::showMcpHelp);
    tools->addSeparator();
    tools->addAction(tr("A&I Models (Recommended for This Computer)..."), this, &MainWindow::aiModelsDialog);
    m_aiSwitchMenu = tools->addMenu(tr("S&witch AI Model"));
    m_aiSwitchMenu->setToolTipsVisible(true);
    connect(m_aiSwitchMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildAiSwitchMenu);
    tools->addSeparator();
    tools->addAction(tr("&Ask Claude..."), this, &MainWindow::showClaude);

    // Help
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About"), this, [this] {
        QMessageBox::about(this, tr("About Video Editor"),
            tr("<h3>Video Editor %1</h3>"
               "<p>A basic non-linear video editor.</p>"
               "<p>Qt %2 &middot; FFmpeg (libavcodec %3) &middot; OpenGL / Vulkan GPU compositing &middot; SQLite %4</p>"
               "<p><b>Shortcuts:</b> Space play/pause, Left/Right step frame, S split, Del delete, "
               "Ctrl+wheel zoom timeline.</p>")
                .arg(QApplication::applicationVersion(), QString::fromLatin1(qVersion()),
                     QString::fromLatin1(LIBAVCODEC_IDENT).section(QLatin1Char('-'), -1),
                     QString::fromLatin1(sqlite3_libversion())));
    });
}

bool MainWindow::setRenderer(RenderBackend backend, bool interactive)
{
    std::unique_ptr<PreviewView> view;
    if (backend == RenderBackend::Vulkan) {
#ifdef VE_HAVE_VULKAN
        QString err;
        if (QVulkanInstance* inst = sharedVulkanInstance(&err)) {
            auto vk = std::make_unique<VulkanPreview>(inst, QSettings().value(QStringLiteral("gpu/vulkanDevice")).toString());
            if (vk->hasDevice()) view = std::move(vk);
            else err = tr("No Vulkan-capable GPU found.");
        }
        if (!view && interactive)
            QMessageBox::warning(this, tr("Vulkan"), tr("Vulkan is not available (%1). Falling back to OpenGL.").arg(err));
#else
        if (interactive) QMessageBox::information(this, tr("Vulkan"), tr("This build was compiled without Vulkan support."));
#endif
    }
    if (!view) view = std::make_unique<GLPreviewWidget>();

    if (m_preview) {
        if (m_interactor) m_interactor->attach(nullptr);
        m_previewLayout->removeWidget(m_preview->widget());
        m_preview.reset();
    }
    m_preview = std::move(view);
    m_previewLayout->insertWidget(0, m_preview->widget(), 1);
    if (m_interactor) m_interactor->attach(m_preview.get());
    m_preview->setFrame(m_interactor ? m_interactor->decorate(m_lastFrame) : m_lastFrame);

    const bool vk = m_preview->backend() == RenderBackend::Vulkan;
    m_actOpenGL->setChecked(!vk);
    m_actVulkan->setChecked(vk);
    QSettings().setValue(QStringLiteral("renderer"), vk ? QStringLiteral("vulkan") : QStringLiteral("opengl"));
    // The backend fills in its GPU description once it has initialised.
    QTimer::singleShot(300, this, &MainWindow::updateStatus);
    QTimer::singleShot(1500, this, &MainWindow::updateStatus);
    updateStatus();
    return m_preview->backend() == backend;
}

// ---------------------------------------------------------------------------
// EditorHost (API / MCP)
// ---------------------------------------------------------------------------
TimeUs MainWindow::playhead() const { return m_playback->position(); }
void MainWindow::seek(TimeUs t) { m_playback->seek(t); }
void MainWindow::setPlaying(bool playing) { playing ? m_playback->play() : m_playback->pause(); }
bool MainWindow::isPlaying() const { return m_playback->isPlaying(); }
QString MainWindow::selectedClip() const { return m_timeline->selectedClip(); }
void MainWindow::selectClip(const QString& id) { m_timeline->setSelectedClip(id); }

QString MainWindow::previewRenderer() const
{
    if (!m_preview) return {};
    return (m_preview->backend() == RenderBackend::Vulkan ? QStringLiteral("vulkan: ") : QStringLiteral("opengl: ")) +
           m_preview->backendDescription();
}

bool MainWindow::setPreviewRenderer(const QString& name, QString* error)
{
    const QString n = name.toLower();
    if (n != QLatin1String("opengl") && n != QLatin1String("vulkan")) {
        if (error) *error = tr("renderer must be 'opengl' or 'vulkan'");
        return false;
    }
    const bool ok = setRenderer(n == QLatin1String("vulkan") ? RenderBackend::Vulkan : RenderBackend::OpenGL, false);
    if (!ok && error) *error = tr("Vulkan is not available on this machine/build; still using OpenGL");
    return ok;
}

void MainWindow::projectReplaced()
{
    generateThumbnails();
    m_project->setModified(false);
    m_timeline->setSelectedClip({});
    m_timeline->zoomToFit();
    if (!m_project->filePath().isEmpty()) addRecent(m_project->filePath());
    updateTitle();
}

void MainWindow::decodeSettingsChanged()
{
    m_engine->resetDecoders();
    m_playback->refresh();
}

void MainWindow::setMcpEnabled(bool enabled)
{
    if (!enabled) {
        m_mcpHttp->close();
        m_statusMcp->setText(tr("MCP: off"));
        return;
    }
    QString err;
    if (m_mcpHttp->listen(m_mcpPort, &err)) {
        m_statusMcp->setText(tr("MCP: %1").arg(m_mcpHttp->url()));
        m_statusMcp->setToolTip(tr("Claude can control this editor through %1 tools at this endpoint.").arg(m_mcp->toolCount()));
    } else {
        m_statusMcp->setText(tr("MCP: port %1 busy").arg(m_mcpPort));
        m_statusMcp->setToolTip(err);
    }
}

void MainWindow::showMcpHelp()
{
    const QString url = m_mcpHttp->url();
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + QStringLiteral("/VideoEditorMCP.exe"));
    QString tools;
    for (const auto& t : m_mcp->toolDefinitions()) tools += t.toObject()["name"].toString() + QStringLiteral(", ");
    tools.chop(2);
    const QString html = tr(
        "<h3>Control this editor with Claude (MCP)</h3>"
        "<p><b>This window</b> (live, HTTP): <code>%1</code></p>"
        "<p><b>Headless</b> (stdio, no window): <code>%2</code></p>"
        "<h4>Claude Code</h4><pre>claude mcp add --transport http video-editor %1</pre>"
        "<h4>Claude Agent SDK (TypeScript)</h4><pre>mcpServers: {\n  \"video-editor\": { type: \"http\", url: \"%1\" }\n}</pre>"
        "<h4>Claude Agent SDK (Python)</h4><pre>mcp_servers={\"video-editor\": {\"type\": \"http\", \"url\": \"%1\"}}</pre>"
        "<p>%3 tools: %4</p>").arg(url.toHtmlEscaped(), exe.toHtmlEscaped()).arg(m_mcp->toolCount()).arg(tools);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("MCP Setup"));
    auto* text = new QTextBrowser(&dlg);
    text->setHtml(html);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(text);
    layout->addWidget(buttons);
    dlg.resize(640, 520);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// Status / title
// ---------------------------------------------------------------------------
void MainWindow::updateTitle()
{
    const QString name = m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).fileName();
    setWindowTitle(tr("%1[*] - Video Editor").arg(name));
    setWindowModified(m_project->isModified());
}

void MainWindow::updateTimecode()
{
    const double fps = m_project->settings().fps;
    m_timecode->setText(QStringLiteral("%1 / %2").arg(formatTimecode(m_playback->position(), fps),
                                                       formatTimecode(m_project->duration(), fps)));
}

void MainWindow::updateStatus()
{
    if (m_preview) m_statusRenderer->setText(tr("Preview: %1").arg(m_preview->backendDescription()));
    const auto& s = m_project->settings();
    m_statusProject->setText(tr("%1 x %2 @ %3 fps%4").arg(s.width).arg(s.height).arg(s.fps, 0, 'g', 5)
                                 .arg(m_playback->hasAudio() ? QString() : tr("  (no audio output)")));
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
bool MainWindow::maybeSave()
{
    if (!m_project->isModified()) return true;
    const auto r = QMessageBox::warning(this, tr("Unsaved changes"), tr("The project has unsaved changes. Save them?"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Save) return save();
    return r == QMessageBox::Discard;
}

bool MainWindow::save()
{
    if (m_project->filePath().isEmpty()) return saveAs();
    QString err;
    if (!ProjectStore::save(*m_project, m_project->filePath(), &err)) {
        QMessageBox::critical(this, tr("Save failed"), err);
        return false;
    }
    m_project->undoStack()->setClean();
    m_project->setModified(false);
    addRecent(m_project->filePath());
    statusBar()->showMessage(tr("Saved %1").arg(m_project->filePath()), 3000);
    updateTitle();
    return true;
}

bool MainWindow::saveAs()
{
    QString dir = m_project->filePath();
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/Untitled.veproj");
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project"), dir, kProjectFilter, &selectedFilter);
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty())
        path += selectedFilter.contains(QLatin1String("json")) ? QStringLiteral(".json") : QStringLiteral(".veproj");
    m_project->setFilePath(path);
    return save();
}

void MainWindow::newProject()
{
    if (!maybeSave()) return;
    m_playback->stop();
    m_project->clear();
    m_timeline->setSelectedClip({});
    updateTitle();
}

void MainWindow::openDialog()
{
    if (!maybeSave()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"), QString(), kProjectFilter);
    if (!path.isEmpty()) openProject(path);
}

bool MainWindow::openProject(const QString& path)
{
    m_playback->stop();
    QString err;
    if (!ProjectStore::load(*m_project, path, &err)) {
        QMessageBox::critical(this, tr("Open failed"), tr("Could not open %1:\n%2").arg(path, err));
        return false;
    }
    QStringList missing;
    for (const auto& m : m_project->media())
        if (!QFileInfo::exists(m.path)) missing << m.path;
    if (!missing.isEmpty())
        QMessageBox::warning(this, tr("Missing media"), tr("These files could not be found:\n%1").arg(missing.join('\n')));
    projectReplaced();
    return true;
}

void MainWindow::generateThumbnails()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const auto media = m_project->media(); // copy: setMediaThumbnail emits signals
    for (const auto& m : media)
        if (QFileInfo::exists(m.path)) m_project->setMediaThumbnail(m.id, MediaProbe::thumbnail(m.path, m.info));
    QApplication::restoreOverrideCursor();
}

void MainWindow::importDialog()
{
    const QString dir = QSettings().value(QStringLiteral("lastImportDir")).toString();
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Media"), dir, kMediaFilter);
    if (files.isEmpty()) return;
    QSettings().setValue(QStringLiteral("lastImportDir"), QFileInfo(files.first()).absolutePath());
    importFiles(files);
}

void MainWindow::importFiles(const QStringList& paths, int track, TimeUs at)
{
    QStringList media;
    for (const QString& p : paths) {
        const QString suffix = QFileInfo(p).suffix().toLower();
        if (suffix == QLatin1String("veproj") || suffix == QLatin1String("json")) {
            if (maybeSave()) openProject(p);
            return;
        }
        media << p;
    }
    if (media.isEmpty()) return;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QJsonObject result;
    try {
        result = m_api->mediaImport(media);
    } catch (const ApiError& e) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, tr("Import"), e.message());
        return;
    }
    QApplication::restoreOverrideCursor();

    // Dropped onto the timeline: place the clips one after another.
    const QJsonArray imported = result["imported"].toArray();
    if (track >= 0 && !imported.isEmpty()) {
        QString last;
        m_project->edit(tr("Add clips"), [&] {
            TimeUs t = std::max<TimeUs>(0, at);
            for (const auto& v : imported) {
                last = m_project->addClipFromMedia(v.toObject()["id"].toString(), std::min(track, m_project->trackCount() - 1), t,
                                                   m_actLinkOnAdd->isChecked());
                if (const Clip* c = m_project->findClip(last)) t = c->end();
            }
        });
        if (!last.isEmpty()) m_timeline->setSelectedClip(last);
    }

    QStringList failed;
    for (const auto& v : result["errors"].toArray())
        failed << QStringLiteral("%1: %2").arg(QFileInfo(v.toObject()["path"].toString()).fileName(), v.toObject()["error"].toString());
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Import"), tr("Some files could not be imported:\n%1").arg(failed.join('\n')));
}

void MainWindow::removeSelectedMedia()
{
    const QString id = m_mediaBin->selectedMediaId();
    if (id.isEmpty()) return;
    if (m_project->isMediaUsed(id)) {
        QMessageBox::information(this, tr("Remove media"), tr("This media is used on the timeline. Delete its clips first."));
        return;
    }
    m_project->removeMedia(id);
}

void MainWindow::addRecent(const QString& path)
{
    QSettings s;
    QStringList recent = s.value(QStringLiteral("recent")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecent) recent.removeLast();
    s.setValue(QStringLiteral("recent"), recent);
    rebuildRecentMenu();
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(QStringLiteral("recent")).toStringList();
    for (const QString& p : recent) {
        m_recentMenu->addAction(QFileInfo(p).fileName(), this, [this, p] {
            if (maybeSave()) openProject(p);
        })->setToolTip(p);
    }
    m_recentMenu->setEnabled(!recent.isEmpty());
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------
void MainWindow::splitAtPlayhead()
{
    const TimeUs t = m_playback->position();
    const QString sel = m_timeline->selectedClip();
    m_project->edit(tr("Split"), [&] {
        const Clip* c = sel.isEmpty() ? nullptr : m_project->findClip(sel);
        if (c && c->contains(t)) {
            m_project->splitClip(sel, t, m_timeline->linkedSelection());
            return;
        }
        // Nothing selected under the playhead: split every clip it crosses.
        QStringList ids;
        for (const auto& tr : m_project->tracks())
            for (const auto& clip : tr.clips)
                if (clip.contains(t)) ids << clip.id;
        for (const QString& id : ids) m_project->splitClip(id, t);
    });
}

void MainWindow::deleteSelected()
{
    const QString sel = m_timeline->selectedClip();
    if (sel.isEmpty()) return;
    m_project->edit(tr("Delete clip"), [&] { m_project->removeClip(sel, m_timeline->linkedSelection()); });
}

bool MainWindow::reportProgress(const QString& text, double fraction)
{
    if (!m_progress) {
        m_progress = new QProgressDialog(this);
        m_progress->setWindowTitle(tr("Working..."));
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(0);
        m_progress->setMinimumWidth(460);
        m_progress->setAutoClose(false);
        m_progress->setAutoReset(false);
        m_progress->setRange(0, 1000);
        m_progress->show();
    }
    if (!text.isEmpty()) m_progress->setLabelText(text);
    if (fraction < 0) m_progress->setRange(0, 0);              // busy indicator
    else {
        if (m_progress->maximum() == 0) m_progress->setRange(0, 1000);
        m_progress->setValue(int(std::clamp(fraction, 0.0, 1.0) * 1000));
    }
    return !m_progress->wasCanceled();
}

void MainWindow::progressDone()
{
    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
}

void MainWindow::separateAudio(const QString& clipId)
{
    if (!m_project->findClip(clipId)) return;
    const QStringList choices = {
        tr("Keep both: voice and music on separate audio tracks"),
        tr("Keep only the voice (remove the music)"),
        tr("Keep only the music (remove the voice)")};
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Separate Voice and Music"),
                                                 tr("The AI model splits the clip's sound into voice and music.\n"
                                                    "What should stay on the timeline?"),
                                                 choices, 0, false, &ok);
    if (!ok) return;
    const QString keep = choice == choices[1] ? QStringLiteral("voice") : choice == choices[2] ? QStringLiteral("music")
                                                                                              : QStringLiteral("both");
    if (!checkModelFits(AiTask::Separate)) return;
    bool install = false;
    if (!StemSeparator::isInstalled()) {
        const AiModel* m = ai::find(StemSeparator::model());
        if (QMessageBox::question(this, tr("Install voice/music separation"),
                                  tr("This uses the %1 AI model, which runs on this computer (on the GPU when "
                                     "possible).\n\nIt needs a one-time setup that downloads Demucs and the model "
                                     "(about %2 MB), and PyTorch if it is not already on this computer (up to "
                                     "2.5 GB for the GPU version).\n\nInstall now?")
                                      .arg(m ? m->name : StemSeparator::model())
                                      .arg(m ? int(m->downloadGB * 1000) + 5 : 90)) != QMessageBox::Yes)
            return;
        install = true;
    }
    try {
        const QJsonObject r = m_api->audioSeparate(clipId, keep, install);
        const QString sel = r.contains("voice") ? r["voice"].toObject()["id"].toString() : r["music"].toObject()["id"].toString();
        if (!sel.isEmpty()) m_timeline->setSelectedClip(sel);
        QString msg = r["cached"].toBool() ? tr("Used the earlier separation of this file.")
                                           : tr("Separated in %1 s on the %2.").arg(r["seconds"].toDouble(), 0, 'f', 1)
                                                 .arg(r["device"].toString() == QLatin1String("cuda") ? tr("GPU") : tr("CPU"));
        if (keep == QLatin1String("both")) msg += tr(" Voice and music are now separate clips on the audio tracks.");
        else if (keep == QLatin1String("voice")) msg += tr(" The music was removed.");
        else msg += tr(" The voice was removed.");
        statusBar()->showMessage(msg, 8000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancelled"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Separate Voice and Music"), e.message());
    }
}

bool MainWindow::checkModelFits(AiTask task)
{
    const AiModel* m = ai::find(ai::selected(task));
    if (!m) return true;
    const AiAssessment a = ai::assess(*m);
    if (a.fit != AiFit::TooHeavy && a.fit != AiFit::Unsupported) return true;
    const QString recId = ai::recommended(task);
    const AiModel* rec = ai::find(recId);
    if (!rec || recId == m->id) return true;
    QMessageBox box(QMessageBox::Warning, ai::taskName(task),
                    tr("<p><b>%1</b> is too heavy for this computer: %2.</p><p>The recommended model here is "
                       "<b>%3</b> (%4).</p>").arg(m->name.toHtmlEscaped(), a.reason.toHtmlEscaped(),
                                                  rec->name.toHtmlEscaped(), rec->requirements.toHtmlEscaped()),
                    QMessageBox::NoButton, this);
    QPushButton* use = box.addButton(tr("Use %1").arg(rec->name), QMessageBox::AcceptRole);
    QPushButton* keep = a.fit == AiFit::Unsupported ? nullptr
                                                    : box.addButton(tr("Try %1 Anyway").arg(m->name), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(use);
    box.exec();
    if (box.clickedButton() == use) {
        ai::setSelected(task, recId);
        return true;
    }
    return keep && box.clickedButton() == keep;
}

void MainWindow::rebuildAiSwitchMenu()
{
    m_aiSwitchMenu->clear();
    for (AiTask task : ai::tasks()) {
        m_aiSwitchMenu->addSection(ai::taskName(task));
        const QString current = ai::selected(task);
        auto* group = new QActionGroup(m_aiSwitchMenu);
        int downloaded = 0;
        for (const AiModel& m : ai::models(task)) {
            if (!ai::isInstalled(m.id)) continue;
            ++downloaded;
            const AiAssessment fit = ai::assess(m);
            QString label = m.name;
            if (m.id == ai::recommended(task)) label += tr("  ★ recommended");
            else if (fit.fit != AiFit::Good) label += QStringLiteral("  — ") + ai::fitLabel(fit.fit);
            QAction* a = m_aiSwitchMenu->addAction(label);
            a->setCheckable(true);
            a->setChecked(m.id == current);
            a->setToolTip(fit.reason.isEmpty() ? m.description : fit.reason);
            group->addAction(a);
            const QString id = m.id, name = m.name;
            connect(a, &QAction::triggered, this, [this, task, id, name] {
                ai::setSelected(task, id);
                statusBar()->showMessage(tr("%1 now uses %2").arg(ai::taskName(task), name), 4000);
            });
        }
        if (!ai::isInstalled(current)) {
            const AiModel* m = ai::find(current);
            QAction* a = m_aiSwitchMenu->addAction(downloaded ? tr("Selected: %1 (downloads when first used)").arg(m ? m->name : current)
                                                              : tr("No model downloaded yet (%1 downloads when first used)")
                                                                    .arg(m ? m->name : current));
            a->setEnabled(false);
        }
    }
    m_aiSwitchMenu->addSeparator();
    m_aiSwitchMenu->addAction(tr("Download or Choose Other Models..."), this, &MainWindow::aiModelsDialog);
}

void MainWindow::aiModelsDialog()
{
    AiModelsDialog dlg(AiModelsDialog::Mode::Settings, this);
    if (dlg.exec() == QDialog::Accepted) statusBar()->showMessage(tr("AI model choices saved"), 3000);
}

void MainWindow::upscaleClip(const QString& clipId)
{
    int track = -1;
    const Clip* c = m_project->findClip(clipId, &track);
    const MediaItem* media = c ? m_project->mediaById(c->mediaId) : nullptr;
    if (!c || m_project->tracks()[track].isAudio() || !media || !media->info.hasVideo || media->info.isImage) {
        statusBar()->showMessage(tr("Select a video clip in the timeline to upscale"), 4000);
        return;
    }
    const ProjectSettings& ps = m_project->settings();
    UpscaleDialog dlg(media->info.width, media->info.height, ps.width, ps.height, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString model = dlg.model();
    const AiModel* info = ai::find(model);
    if (!info) return;
    ai::setSelected(AiTask::Upscale, model);

    const AiAssessment fit = ai::assess(*info);
    bool force = false;
    if (fit.fit == AiFit::Unsupported) {
        const AiModel* rec = ai::find(ai::recommended(AiTask::Upscale));
        QMessageBox::warning(this, tr("Upscale Video"), tr("%1 cannot run on this computer: %2.\n\nChoose %3 instead.")
                                                            .arg(info->name, fit.reason, rec ? rec->name : QString()));
        return;
    }
    if (fit.fit == AiFit::TooHeavy) {
        if (!checkModelFits(AiTask::Upscale)) return;
        force = ai::selected(AiTask::Upscale) == model;   // "Try anyway"
        if (!force) {                                      // switched to the recommended model: confirm its settings
            upscaleClip(clipId);
            return;
        }
    }
    bool install = false;
    if (!VideoUpscaler::isInstalled(model)) {
        const bool torch = !VideoUpscaler::isEnvInstalled();
        const QString size = info->downloadGB < 0.1 ? tr("%1 MB").arg(int(info->downloadGB * 1000) + 1)
                                                    : tr("%1 GB").arg(info->downloadGB, 0, 'f', 1);
        if (QMessageBox::question(this, tr("Install %1").arg(info->name),
                                  tr("%1 runs on this computer (%2).\n\nIt needs a one-time download of about %3%4."
                                     "\n\nInstall now?")
                                      .arg(info->name, info->requirements, size,
                                           torch ? tr(" plus PyTorch (about 2.5 GB for the GPU version)") : QString())) != QMessageBox::Yes)
            return;
        install = true;
    }
    try {
        const QJsonObject r = m_api->videoUpscale(clipId, model, dlg.scale(), dlg.replaceClip(), install, force);
        if (dlg.setProjectResolution())
            m_api->projectSetSettings({{"width", r["width"].toInt()}, {"height", r["height"].toInt()}});
        if (r.contains("clip")) m_timeline->setSelectedClip(r["clip"].toObject()["id"].toString());
        statusBar()->showMessage(
            r["cached"].toBool()
                ? tr("Used the earlier upscale (%1 × %2).").arg(r["width"].toInt()).arg(r["height"].toInt())
                : tr("Upscaled to %1 × %2 with %3 in %4 s on the %5.")
                      .arg(r["width"].toInt()).arg(r["height"].toInt()).arg(info->name)
                      .arg(r["seconds"].toDouble(), 0, 'f', 0)
                      .arg(r["device"].toString().startsWith(QLatin1String("cuda")) ? tr("GPU") : tr("CPU")),
            10000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancelled"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Upscale Video"), e.message());
    }
}

void MainWindow::transcribeClip(const QString& clipId, const QString& language, bool isolateVoice, const QString& hints)
{
    if (clipId.isEmpty() || !m_project->findClip(clipId)) {
        statusBar()->showMessage(tr("Select a clip with speech or singing in the timeline first"), 4000);
        return;
    }
    if (!checkModelFits(AiTask::Transcribe)) return;
    bool install = false;
    if (!Transcriber::isInstalled()) {
        const AiModel* m = ai::find(Transcriber::modelId());
        if (QMessageBox::question(this, tr("Install transcription"),
                                  tr("Transcription uses OpenAI's %1 model on this computer (on the GPU "
                                     "when possible).\n\nIt needs a one-time download of about %2 GB (plus PyTorch "
                                     "if voice/music separation is not set up yet).\n\nInstall now?")
                                      .arg(m ? m->name : Transcriber::model())
                                      .arg(Transcriber::downloadGB(), 0, 'f', 1)) != QMessageBox::Yes)
            return;
        install = true;
    }
    try {
        const QJsonObject r = m_api->transcriptCreate(clipId, language, isolateVoice ? QStringLiteral("auto") : QStringLiteral("no"),
                                                      install, hints);
        m_transcriptDock->show();
        m_transcriptDock->raise();
        const int lines = r["lines"].toArray().size();
        statusBar()->showMessage(tr("Transcribed %n line(s) in %1 s%2. Double-click a line in the Transcript panel to edit it.",
                                    nullptr, lines)
                                     .arg(r["seconds"].toDouble(), 0, 'f', 1)
                                     .arg(r["isolatedVoice"].toBool() ? tr(" (voice separated from the music first)") : QString()),
                                 8000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancelled"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Transcribe"), e.message());
    }
}

void MainWindow::autoCaptions()
{
    // Like CapCut's auto captions: transcribe the selected clip (if needed) and show the text on the video.
    const QString sel = m_timeline->selectedClip();
    if (m_project->transcriptLines().empty() || (!sel.isEmpty() && m_project->findClip(sel))) {
        if (sel.isEmpty()) {
            statusBar()->showMessage(tr("Select the clip with the speech or song first"), 4000);
            return;
        }
        bool haveIt = false;
        try {
            const QString audio = m_api->transcriptGet(sel)["transcripts"].toArray().isEmpty() ? QString() : sel;
            haveIt = !audio.isEmpty();
        } catch (const ApiError&) {
        }
        if (!haveIt) {
            transcribeClip(sel);
            if (m_project->transcriptLines().empty()) return;   // cancelled / failed
        }
    }
    CaptionStyle s = m_project->captionStyle();
    s.enabled = true;
    m_project->setCaptionStyle(s);
    m_captionDock->show();
    m_captionDock->raise();
    statusBar()->showMessage(tr("Captions are on. Pick a style in the Captions panel; edit words in the Transcript panel."), 6000);
}

void MainWindow::exportTranscript(const QString& format)
{
    QString base = m_project->filePath().isEmpty() ? QStringLiteral("transcript") : QFileInfo(m_project->filePath()).completeBaseName();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Transcript"), dir + QLatin1Char('/') + base + QLatin1Char('.') + format,
                                                      tr("%1 files (*.%2)").arg(format.toUpper(), format));
    if (path.isEmpty()) return;
    try {
        const QJsonObject r = m_api->transcriptExport(path, format);
        statusBar()->showMessage(tr("Exported %1 lines to %2").arg(r["lines"].toInt()).arg(QDir::toNativeSeparators(path)), 6000);
    } catch (const ApiError& e) {
        QMessageBox::warning(this, tr("Export Transcript"), e.message());
    }
}

void MainWindow::addText(const QString& templateId)
{
    try {
        const QJsonObject r = m_api->textAdd(QJsonObject{{"template", templateId.isEmpty() ? QStringLiteral("title") : templateId}});
        const QString id = r["id"].toString();
        m_timeline->setSelectedClip(id);
        m_inspector->setClip(id);
        m_textPanel->setClip(id);
        m_textDock->show();
        m_textDock->raise();
        statusBar()->showMessage(tr("Text added. Type your words in the Text panel; move it in the Inspector."), 6000);
    } catch (const ApiError& e) {
        QMessageBox::warning(this, tr("Add Text"), e.message());
    }
}

namespace {
// Asks before the first use of an engine that needs a download or sends text online.
bool confirmSpeechEngine(QWidget* parent, const QString& engine, const QString& voice)
{
    if (TextToSpeech::isOnline(engine) && !QSettings().value(QStringLiteral("tts/edgeConsent")).toBool()) {
        if (QMessageBox::question(parent, QObject::tr("Online voices"),
                                  QObject::tr("Microsoft Edge voices run online: the text is sent to Microsoft's speech "
                                              "service to make the audio.\n\nUse them?")) != QMessageBox::Yes)
            return false;
        QSettings().setValue(QStringLiteral("tts/edgeConsent"), true);
    }
    if (!TextToSpeech::isInstalled(engine, engine == QLatin1String("piper") ? voice : QString()) &&
        engine != QLatin1String("system")) {
        const QString what = engine == QLatin1String("piper")
                                 ? QObject::tr("Piper and the voice %1 (about 90 MB the first time, 60 MB per extra voice)").arg(voice)
                                 : QObject::tr("the Edge voices package (about 1 MB)");
        if (QMessageBox::question(parent, QObject::tr("Set up text to speech"),
                                  QObject::tr("This downloads %1 into the editor's private AI environment.\n\nContinue?").arg(what)) !=
            QMessageBox::Yes)
            return false;
    }
    return true;
}
} // namespace

void MainWindow::speakText(const QString& clipId)
{
    const Clip* c = m_project->findClip(clipId);
    const bool fromClip = c && c->isText();
    SpeechDialog dlg(SpeechDialog::Mode::Text, fromClip ? c->text->text : QString(), {}, {}, this);
    if (dlg.exec() != QDialog::Accepted || dlg.text().isEmpty()) return;
    if (!confirmSpeechEngine(this, dlg.engine(), dlg.voice())) return;
    QJsonObject args{{"text", dlg.text()}, {"engine", dlg.engine()}, {"voice", dlg.voice()}, {"rate", dlg.rate()},
                     {"install", true}};
    if (fromClip) args["start"] = usToSeconds(c->start);
    try {
        const QJsonObject r = m_api->ttsSpeak(args);
        statusBar()->showMessage(tr("Voice-over added on %1 (%2 s).")
                                     .arg(m_project->tracks()[r["track"].toInt()].name)
                                     .arg(r["duration"].toDouble(), 0, 'f', 1),
                                 6000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancelled"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Text to Speech"), e.message());
    }
}

void MainWindow::voiceOverSubtitles()
{
    if (m_project->transcriptLines().empty()) {
        QMessageBox::information(this, tr("Voice-over from Subtitles"),
                                 tr("There are no subtitles yet: transcribe a clip or import subtitles first."));
        return;
    }
    QStringList langs{QString()};
    langs += m_project->translationLanguages();
    QString transcriptLang;
    if (const Transcript* t = m_project->transcript(m_project->transcriptLines().front().mediaId)) transcriptLang = t->language;
    SpeechDialog dlg(SpeechDialog::Mode::Subtitles, {}, langs, transcriptLang, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!confirmSpeechEngine(this, dlg.engine(), dlg.voice())) return;
    QJsonObject args{{"language", dlg.language()}, {"engine", dlg.engine()}, {"voice", dlg.voice()}, {"rate", dlg.rate()},
                     {"install", true}};
    if (dlg.lowerOriginal()) args["originalVolume"] = dlg.originalVolume();
    try {
        const QJsonObject r = m_api->ttsSubtitles(args);
        statusBar()->showMessage(tr("Voice-over of %n line(s) added on the Voice-over track.", nullptr, r["lines"].toInt()), 8000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancel"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Voice-over from Subtitles"), e.message());
    }
}

void MainWindow::importSubtitles()
{
    const QString sel = m_timeline->selectedClip();
    if (sel.isEmpty() || !m_project->findClip(sel)) {
        QMessageBox::information(this, tr("Import Subtitles"),
                                 tr("Select the clip the subtitles belong to (its sound) in the timeline first."));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Subtitles"),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                      tr("Subtitles (*.srt *.vtt *.lrc)"));
    if (path.isEmpty()) return;
    bool replace = true;
    {
        bool hasTranscript = false;
        try {
            hasTranscript = !m_api->transcriptGet(sel)["transcripts"].toArray().isEmpty();
        } catch (const ApiError&) {   // no transcript yet, or no sound linked (reported by the import)
        }
        if (hasTranscript) {
            const auto b = QMessageBox::question(this, tr("Import Subtitles"),
                                                 tr("This clip already has a transcript. Replace it with the file?\n\n"
                                                    "Yes = replace, No = add the subtitles to it."),
                                                 QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            if (b == QMessageBox::Cancel) return;
            replace = b == QMessageBox::Yes;
        }
    }
    try {
        const QJsonObject r = m_api->transcriptImport(sel, path, replace);
        m_transcriptDock->show();
        m_transcriptDock->raise();
        statusBar()->showMessage(tr("Imported %1 subtitle line(s)%2.")
                                     .arg(r["imported"].toInt())
                                     .arg(r["skipped"].toInt() ? tr(" (%1 outside the clip skipped)").arg(r["skipped"].toInt()) : QString()),
                                 6000);
    } catch (const ApiError& e) {
        QMessageBox::warning(this, tr("Import Subtitles"), e.message());
    }
}

void MainWindow::translateSubtitles()
{
    if (m_project->transcriptLines().empty()) {
        QMessageBox::information(this, tr("Translate Subtitles"),
                                 tr("There are no subtitles yet: transcribe a clip or import subtitles first."));
        return;
    }
    QStringList items;
    QStringList codes;
    for (const auto& [code, name] : subtitleLanguages()) {
        items << name;
        codes << code;
    }
    const QString last = QSettings().value(QStringLiteral("translate/last"), QStringLiteral("en")).toString();
    bool ok = false;
    const QString pick = QInputDialog::getItem(this, tr("Translate Subtitles"),
                                               tr("Translate every subtitle line into (offline, NLLB-200):"), items,
                                               std::max(0, int(codes.indexOf(last))), false, &ok);
    if (!ok) return;
    const QString lang = codes[items.indexOf(pick)];
    QSettings().setValue(QStringLiteral("translate/last"), lang);
    if (!checkModelFits(AiTask::Translate)) return;
    bool install = false;
    if (!Translator::isInstalled()) {
        if (QMessageBox::question(this, tr("Install translation"),
                                  tr("Translation runs Meta's NLLB-200 model on this computer (on the GPU when possible).\n\n"
                                     "It needs a one-time download of about %1 GB (plus PyTorch if no other AI feature "
                                     "is set up yet).\n\nInstall now?")
                                      .arg(Translator::downloadGB(), 0, 'f', 1)) != QMessageBox::Yes)
            return;
        install = true;
    }
    try {
        const QJsonObject r = m_api->transcriptTranslate(lang, {}, install, true);
        m_transcriptDock->show();
        m_transcriptDock->raise();
        statusBar()->showMessage(tr("Translated %1 line(s) into %2 in %3 s. The captions now show the translation "
                                    "(Captions panel > Language).")
                                     .arg(r["lines"].toInt())
                                     .arg(languageName(lang))
                                     .arg(r["seconds"].toDouble(), 0, 'f', 1),
                                 8000);
    } catch (const ApiError& e) {
        progressDone();
        if (!e.message().contains(QLatin1String("cancelled"), Qt::CaseInsensitive))
            QMessageBox::warning(this, tr("Translate Subtitles"), e.message());
    }
}

bool MainWindow::requireSelection(QString* id)
{
    *id = m_timeline->selectedClip();
    if (id->isEmpty() || !m_project->findClip(*id)) {
        statusBar()->showMessage(tr("Select a clip in the timeline first"), 3000);
        return false;
    }
    return true;
}

void MainWindow::duplicateSelected()
{
    QString id;
    if (!requireSelection(&id)) return;
    QString copy;
    m_project->edit(tr("Duplicate clip"), [&] { copy = m_project->duplicateClip(id, m_timeline->linkedSelection()); });
    if (!copy.isEmpty()) m_timeline->setSelectedClip(copy);
}

void MainWindow::rippleDeleteSelected()
{
    QString id;
    if (!requireSelection(&id)) return;
    m_project->edit(tr("Ripple delete"), [&] { m_project->rippleDelete(id, m_timeline->linkedSelection()); });
}

void MainWindow::freezeFrameSelected()
{
    QString id;
    if (!requireSelection(&id)) return;
    try {
        const QJsonObject still = m_api->clipFreezeFrame(id, usToSeconds(m_playback->position()), 2.0);
        m_timeline->setSelectedClip(still["id"].toString());
        statusBar()->showMessage(tr("Inserted a 2 s freeze frame (trim it to change the length)"), 4000);
    } catch (const ApiError& e) {
        statusBar()->showMessage(e.message(), 4000);
    }
}

void MainWindow::reverseSelected()
{
    QString id;
    if (!requireSelection(&id)) return;
    const Clip* c = m_project->findClip(id);
    if (c->freeze) return;
    const bool on = !c->reverse;
    m_project->edit(on ? tr("Reverse clip") : tr("Play forward"),
                    [&] { m_project->setClipReverse(id, on, m_timeline->linkedSelection()); });
    statusBar()->showMessage(on ? tr("Clip plays backwards") : tr("Clip plays forwards"), 3000);
}

void MainWindow::setSelectedSpeed(double speed)
{
    QString id;
    if (!requireSelection(&id)) return;
    const Clip* c = m_project->findClip(id);
    if (c->freeze) return;
    const auto curve = c->speedCurve;
    const QString preset = c->curvePreset;
    m_project->edit(tr("Change speed"), [&] { m_project->setClipSpeed(id, speed, curve, preset, m_timeline->linkedSelection()); });
}

void MainWindow::customSpeedDialog()
{
    QString id;
    if (!requireSelection(&id)) return;
    bool ok = false;
    const double v = QInputDialog::getDouble(this, tr("Clip Speed"), tr("Speed (0.5 = slow motion, 2 = twice as fast):"),
                                             m_project->findClip(id)->speed, 0.05, 16.0, 2, &ok);
    if (ok) setSelectedSpeed(v);
}

void MainWindow::setSelectedCurve(const QString& preset)
{
    QString id;
    if (!requireSelection(&id)) return;
    const Clip* c = m_project->findClip(id);
    if (c->freeze) return;
    const double speed = c->speed;
    m_project->edit(tr("Speed curve"), [&] {
        m_project->setClipSpeed(id, speed, speedCurvePreset(preset), preset == QLatin1String("none") ? QString() : preset,
                                m_timeline->linkedSelection());
    });
}

void MainWindow::fitSelected(bool fill)
{
    QString id;
    if (!requireSelection(&id)) return;
    try {
        m_api->clipUpdate(id, {{"fit", fill ? "fill" : "fit"}});
    } catch (const ApiError& e) {
        statusBar()->showMessage(e.message(), 4000);
    }
}

void MainWindow::rotateSelected(double degrees)
{
    QString id;
    if (!requireSelection(&id)) return;
    Clip c = *m_project->findClip(id);
    double r = std::fmod(c.transform.rotation + degrees, 360.0);
    if (r > 180) r -= 360;
    if (r <= -180) r += 360;
    c.transform.rotation = r;
    m_project->edit(tr("Rotate clip"), [&] { m_project->updateClip(c); });
}

void MainWindow::cutRange(bool closeGap)
{
    if (!m_timeline->hasRange()) {
        statusBar()->showMessage(tr("Mark a range first: I at the start, O at the end"), 4000);
        return;
    }
    const TimeUs a = m_timeline->markIn(), b = m_timeline->markOut();
    m_project->edit(closeGap ? tr("Cut range") : tr("Delete range"), [&] { m_project->removeRange(a, b, closeGap); });
    m_timeline->clearMarks();
    m_playback->seek(a);
}

void MainWindow::closeGapsAtPlayhead()
{
    const TimeUs t = m_playback->position();
    TimeUs closed = 0;
    m_project->edit(tr("Close gap"), [&] {
        for (int i = 0; i < m_project->trackCount(); ++i) closed = std::max(closed, m_project->closeGap(i, t));
    });
    statusBar()->showMessage(closed > 0 ? tr("Closed a %1 s gap").arg(usToSeconds(closed), 0, 'f', 2)
                                        : tr("No gap at the playhead"), 3000);
}

void MainWindow::toggleLinkSelected()
{
    const QString sel = m_timeline->selectedClip();
    if (sel.isEmpty() || !m_project->findClip(sel)) {
        statusBar()->showMessage(tr("Select a video or audio clip first"), 3000);
        return;
    }
    if (!m_project->linkedClips(sel).isEmpty()) {
        m_project->edit(tr("Unlink audio and video"), [&] { m_project->unlinkClip(sel); });
        statusBar()->showMessage(tr("Audio unlinked from the video: each can now be moved and trimmed on its own"), 4000);
    } else if (const QString mate = m_project->linkCandidate(sel); !mate.isEmpty()) {
        m_project->edit(tr("Link audio and video"), [&] { m_project->linkClips({sel, mate}); });
        statusBar()->showMessage(tr("Audio and video linked again"), 4000);
    } else {
        statusBar()->showMessage(tr("This clip has no matching audio/video clip to link with"), 4000);
    }
}

void MainWindow::updateLinkActions()
{
    if (!m_actLinkToggle) return;
    const QString sel = m_timeline->selectedClip();
    const bool linked = !sel.isEmpty() && !m_project->linkedClips(sel).isEmpty();
    const bool canLink = !sel.isEmpty() && !linked && !m_project->linkCandidate(sel).isEmpty();
    m_actLinkToggle->setText(canLink ? tr("&Link Audio and Video") : tr("&Unlink Audio and Video"));
    m_actLinkToggle->setEnabled(linked || canLink);
}

void MainWindow::addMediaAtPlayhead(const QString& mediaId)
{
    try {
        const QJsonObject clip = m_api->clipAdd(mediaId, -1, -1, -1, m_actLinkOnAdd->isChecked()); // at the playhead
        m_timeline->setSelectedClip(clip["id"].toString());
    } catch (const ApiError& e) {
        statusBar()->showMessage(e.message(), 4000);
    }
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
void MainWindow::projectSettingsDialog()
{
    ProjectSettings s = m_project->settings();
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Project Settings - Canvas Size / Aspect Ratio"));
    auto* form = new QFormLayout;
    auto* w = new QSpinBox(&dlg);
    w->setRange(16, 8192);
    w->setSingleStep(2);
    w->setValue(s.width);
    auto* h = new QSpinBox(&dlg);
    h->setRange(16, 8192);
    h->setSingleStep(2);
    h->setValue(s.height);
    auto* presets = new QComboBox(&dlg);
    presets->addItem(tr("Custom"));
    const QList<QPair<QString, QSize>> sizes = {
        {tr("16:9 Landscape 4K"), {3840, 2160}}, {tr("16:9 Landscape 1440p"), {2560, 1440}},
        {tr("16:9 Landscape 1080p (YouTube)"), {1920, 1080}}, {tr("16:9 Landscape 720p"), {1280, 720}},
        {tr("9:16 Vertical (TikTok, Reels, Shorts)"), {1080, 1920}}, {tr("1:1 Square"), {1080, 1080}},
        {tr("4:5 Portrait (Instagram)"), {1080, 1350}}, {tr("4:3 Classic"), {1440, 1080}},
        {tr("21:9 Cinema"), {2560, 1080}}, {tr("16:9 480p"), {854, 480}}};
    for (const auto& [label, sz] : sizes)
        presets->addItem(QStringLiteral("%1  (%2 x %3)").arg(label).arg(sz.width()).arg(sz.height()), sz);
    connect(presets, &QComboBox::currentIndexChanged, &dlg, [=](int i) {
        if (i <= 0) return;
        const QSize sz = presets->itemData(i).toSize();
        w->setValue(sz.width());
        h->setValue(sz.height());
    });
    auto* fps = new QComboBox(&dlg);
    fps->setEditable(true);
    for (const char* f : {"23.976", "24", "25", "29.97", "30", "50", "59.94", "60"}) fps->addItem(QString::fromLatin1(f));
    fps->setCurrentText(QString::number(s.fps, 'g', 6));
    auto* bg = new QPushButton(&dlg);
    QColor bgColor = s.background;
    auto paintBg = [bg, &bgColor] { bg->setStyleSheet(QStringLiteral("background:%1; min-height:20px;").arg(bgColor.name())); };
    paintBg();
    connect(bg, &QPushButton::clicked, &dlg, [&] {
        const QColor c = QColorDialog::getColor(bgColor, &dlg, tr("Background"));
        if (c.isValid()) { bgColor = c; paintBg(); }
    });
    form->addRow(tr("Preset"), presets);
    form->addRow(tr("Width"), w);
    form->addRow(tr("Height"), h);
    form->addRow(tr("Frame rate"), fps);
    form->addRow(tr("Background"), bg);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addLayout(form);
    layout->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted) return;

    s.width = w->value() & ~1;
    s.height = h->value() & ~1;
    bool ok = false;
    const double f = fps->currentText().toDouble(&ok);
    if (ok && f >= 1 && f <= 240) s.fps = f;
    s.background = bgColor;
    m_project->setSettings(s);
}

void MainWindow::gpuSettingsDialog()
{
    QSettings s;
    const QString oldDecode = hw::decodeMode();
    const QString oldVk = s.value(QStringLiteral("gpu/vulkanDevice")).toString();
    GpuSettingsDialog dlg(oldDecode, oldVk, this);
    if (dlg.exec() != QDialog::Accepted) return;

    if (dlg.decodeMode() != oldDecode) {
        hw::setDecodeMode(dlg.decodeMode());
        s.setValue(QStringLiteral("gpu/decode"), dlg.decodeMode());
        m_engine->resetDecoders();     // reopen decoders with the new mode
        m_playback->refresh();
    }
    if (dlg.vulkanDevice() != oldVk) {
        s.setValue(QStringLiteral("gpu/vulkanDevice"), dlg.vulkanDevice());
        if (m_preview && m_preview->backend() == RenderBackend::Vulkan) setRenderer(RenderBackend::Vulkan);
    }
}

void MainWindow::updateDecodeStatus(const CompositeFramePtr& frame)
{
    if (!frame) return;
    int total = 0, gpu = 0;
    const char* device = nullptr;
    for (const auto& l : frame->layers) {
        if (!l.frame) continue;
        ++total;
        if (l.frame->hwDevice) { ++gpu; device = l.frame->hwDevice; }
    }
    QString text;
    if (total == 0) text = hw::decodeMode() == QLatin1String("none") ? tr("Decode: CPU") : tr("Decode: GPU ready");
    else if (gpu == total) text = tr("Decode: GPU (%1)").arg(QString::fromLatin1(device));
    else if (gpu == 0) text = tr("Decode: CPU");
    else text = tr("Decode: GPU %1/%2 layers (%3)").arg(gpu).arg(total).arg(QString::fromLatin1(device));
    if (m_statusDecode->text() != text) m_statusDecode->setText(text);
}

void MainWindow::exportVideo()
{
    if (m_project->duration() <= 0) {
        QMessageBox::information(this, tr("Export"), tr("The timeline is empty."));
        return;
    }
    m_playback->pause();
    QString suggested = m_project->filePath().isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/export.mp4")
        : QFileInfo(m_project->filePath()).absolutePath() + QLatin1Char('/') + QFileInfo(m_project->filePath()).completeBaseName() + QStringLiteral(".mp4");
    ExportDialog dlg(m_project->settings(), suggested, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ExportSettings settings = dlg.settings();
    if (settings.path.isEmpty()) return;

    QProgressDialog progress(tr("Exporting video..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setValue(0);

    const TimelineSnapshot snapshot = m_project->snapshot();
    Exporter exporter;
    QString err;
    QElapsedTimer timer;
    timer.start();
    const bool ok = exporter.run(snapshot, settings, [&](double f) {
        progress.setValue(int(f * 1000));
        QApplication::processEvents();
        return !progress.wasCanceled();
    }, &err);
    progress.close();

    if (ok) {
        QMessageBox::information(this, tr("Export"), tr("Exported %1 in %2 s.").arg(settings.path).arg(timer.elapsed() / 1000.0, 0, 'f', 1));
    } else {
        QMessageBox::warning(this, tr("Export"), err);
    }
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    m_playback->pause();
    QSettings s;
    s.setValue(QStringLiteral("window/geometry"), saveGeometry());
    s.setValue(QStringLiteral("window/state"), saveState(kWindowStateVersion));
    e->accept();
}

} // namespace ve
