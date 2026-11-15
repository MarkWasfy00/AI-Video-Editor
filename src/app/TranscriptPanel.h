#pragma once

#include "core/Project.h"

#include <QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace ve {

// Transcript / lyrics of everything on the timeline. Shows the line being
// heard karaoke-style (current word highlighted), lists every line with its
// timeline time, lets you edit the text, and exports SRT / VTT / LRC / TXT.
class TranscriptPanel : public QWidget {
    Q_OBJECT
public:
    struct Host {
        std::function<void(TimeUs)> seek;
        std::function<QString()> selectedClip;
        std::function<void(const QString& clipId, const QString& language, bool isolateVoice, const QString& hints)> transcribe;
        std::function<void(const QString& format)> exportAs;
    };

    TranscriptPanel(Project* project, Host host, QWidget* parent = nullptr);

public slots:
    void setPlayhead(ve::TimeUs t);

private:
    void rebuild();
    void updateCurrent();
    int lineAt(TimeUs t) const;
    bool isEditing() const;

    Project* m_project;
    Host m_host;
    QPushButton* m_transcribe = nullptr;
    QComboBox* m_language = nullptr;
    QCheckBox* m_isolate = nullptr;
    QLineEdit* m_hints = nullptr;
    QToolButton* m_export = nullptr;
    QCheckBox* m_follow = nullptr;
    QCheckBox* m_captions = nullptr;
    QLabel* m_lyric = nullptr;
    QLabel* m_hint = nullptr;
    QTableWidget* m_table = nullptr;
    std::vector<TimelineLine> m_lines;
    TimeUs m_playhead = 0;
    int m_current = -1;
    bool m_rebuilding = false;
    bool m_pendingRebuild = false;
};

} // namespace ve
