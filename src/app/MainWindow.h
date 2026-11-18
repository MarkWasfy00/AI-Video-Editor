#pragma once

#include "media/AiModels.h"
#include "api/EditorApi.h"
#include "render/PreviewView.h"

#include <QMainWindow>

#include <QHash>
#include <QSet>

#include <atomic>
#include <memory>

class QAction;
class QDockWidget;
class QProgressDialog;
class QLabel;
class QMenu;
class QToolButton;
class QVBoxLayout;

namespace ve {

class ClaudePanel;
class TranscriptPanel;
class CaptionPanel;
class TextPanel;
class PreviewInteractor;
class Inspector;
class McpHttpTransport;
class McpServer;
class MediaBin;
class MixerPanel;
class PlaybackController;
class PreviewEngine;
class Project;
class TimelineWidget;

class MainWindow : public QMainWindow, public EditorHost {
    Q_OBJECT
public:
    // mcpPort: 0 = use the saved setting (default 8765); -1 = disable MCP.
    explicit MainWindow(RenderBackend backend, int mcpPort = 0, QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString& path);
    void importFiles(const QStringList& paths, int track = -1, TimeUs at = -1);

    // EditorHost (used by the API / MCP tools)
    QString hostKind() const override { return QStringLiteral("gui"); }
    TimeUs playhead() const override;
    void seek(TimeUs t) override;
    void setPlaying(bool playing) override;
    bool isPlaying() const override;
    QString selectedClip() const override;
    void selectClip(const QString& id) override;
    QString previewRenderer() const override;
    bool setPreviewRenderer(const QString& name, QString* error) override;
    void projectReplaced() override;
    void decodeSettingsChanged() override;
    bool wantsThumbnails() const override { return true; }
    bool audioLevels(MixLevels& out) const override;
    bool reportProgress(const QString& text, double fraction) override;
    void progressDone() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void createActions();
    void createLayout();
    bool setRenderer(RenderBackend backend, bool interactive = true);
    void setMcpEnabled(bool enabled);
    void showMcpHelp();
    enum class PanelLayout { SideBySide, Stacked, Tabbed };
    void arrangeInspectorAndMixer(PanelLayout layout);
    void resetPanelLayout();          // default: media top-left, timeline bottom
    void showClaude();
    QString claudeMcpUrl();           // starts the MCP endpoint if needed
    QString editorStateForClaude() const;

    bool maybeSave();
    bool save();
    bool saveAs();
    void newProject();
    void openDialog();
    void importDialog();
    void exportVideo();
    void projectSettingsDialog();
    void gpuSettingsDialog();
    void updateDecodeStatus(const CompositeFramePtr& frame);

    void splitAtPlayhead();
    void deleteSelected();
    void toggleLinkSelected();         // Ctrl+L: unlink / re-link the selected clip's audio and video
    // Clip menu (act on the selected clip)
    void duplicateSelected();
    void rippleDeleteSelected();
    void freezeFrameSelected();
    void reverseSelected();
    void setSelectedSpeed(double speed);
    void customSpeedDialog();
    void setSelectedCurve(const QString& preset);
    void fitSelected(bool fill);
    void rotateSelected(double degrees);
    // Range / gaps
    void cutRange(bool closeGap);
    void closeGapsAtPlayhead();
    bool requireSelection(QString* id);
    void separateAudio(const QString& clipId);   // voice / music separation (asks what to keep)
    void upscaleClip(const QString& clipId);     // AI super-resolution (FlashVSR / Real-ESRGAN)
    void aiModelsDialog();                       // Tools > AI Models
    // Before an AI task: if the chosen model is too heavy for this computer, offers the
    // recommended one. Returns false if the user cancelled.
    bool checkModelFits(AiTask task);
    void transcribeClip(const QString& clipId, const QString& language = QStringLiteral("auto"), bool isolateVoice = true,
                        const QString& hints = {});
    void exportTranscript(const QString& format);
    void autoCaptions();                       // transcribe the selected clip + show captions
    // Text overlays, subtitles and speech
    void addText(const QString& templateId);   // at the playhead, selected afterwards
    void speakText(const QString& clipId);     // text to speech (a text clip's words, or typed)
    void voiceOverSubtitles();                 // speak every subtitle line (dubbing)
    void importSubtitles();
    void translateSubtitles();
    void updateLinkActions();
    void addMediaAtPlayhead(const QString& mediaId);
    void removeSelectedMedia();
    void generateThumbnails();
    void ensureWaveforms();            // computes missing audio waveforms in the background

    void updateTitle();
    void updateTimecode();
    void updateStatus();
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void rebuildAiSwitchMenu();                  // Tools > Switch AI Model (downloaded models)

    Project* m_project = nullptr;
    PreviewEngine* m_engine = nullptr;
    PlaybackController* m_playback = nullptr;

    std::unique_ptr<PreviewView> m_preview;
    QVBoxLayout* m_previewLayout = nullptr;
    CompositeFramePtr m_lastFrame;

    MediaBin* m_mediaBin = nullptr;
    TimelineWidget* m_timeline = nullptr;
    Inspector* m_inspector = nullptr;
    MixerPanel* m_mixer = nullptr;
    QDockWidget* m_mixerDock = nullptr;
    QDockWidget* m_inspectorDock = nullptr;
    QProgressDialog* m_progress = nullptr;
    QDockWidget* m_mediaDock = nullptr;
    QDockWidget* m_timelineDock = nullptr;
    ClaudePanel* m_claude = nullptr;
    QDockWidget* m_claudeDock = nullptr;
    TranscriptPanel* m_transcript = nullptr;
    QDockWidget* m_transcriptDock = nullptr;
    CaptionPanel* m_captionPanel = nullptr;
    QDockWidget* m_captionDock = nullptr;
    TextPanel* m_textPanel = nullptr;
    PreviewInteractor* m_interactor = nullptr;
    QDockWidget* m_textDock = nullptr;
    MixLevels m_lastLevels;            // latest meter block (for mixer_get)
    QHash<QString, std::shared_ptr<const std::vector<float>>> m_waveCache; // by file path
    QSet<QString> m_wavePending;
    std::shared_ptr<std::atomic<bool>> m_waveCancel = std::make_shared<std::atomic<bool>>(false);

    QLabel* m_timecode = nullptr;
    QLabel* m_statusRenderer = nullptr;
    QLabel* m_statusProject = nullptr;
    QLabel* m_statusDecode = nullptr;
    QLabel* m_statusMcp = nullptr;
    QAction* m_actMcp = nullptr;
    QAction* m_actLinkToggle = nullptr;     // Unlink / Link Audio and Video
    QAction* m_actLinkedSelection = nullptr;
    QAction* m_actLinkOnAdd = nullptr;
    std::unique_ptr<EditorApi> m_api;
    McpServer* m_mcp = nullptr;
    McpHttpTransport* m_mcpHttp = nullptr;
    quint16 m_mcpPort = 8765;
    QToolButton* m_playButton = nullptr;
    QAction* m_actPlay = nullptr;
    QAction* m_actOpenGL = nullptr;
    QAction* m_actVulkan = nullptr;
    QMenu* m_recentMenu = nullptr;
    QMenu* m_aiSwitchMenu = nullptr;
};

} // namespace ve
