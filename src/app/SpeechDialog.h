#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QSlider;

namespace ve {

// Text to speech: the words (or every subtitle line), the engine, the voice
// and the speed. Mode::Subtitles makes a voice-over of the subtitles
// (dubbing when a translation is chosen).
class SpeechDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Text, Subtitles };
    // languages (Subtitles): "" (the transcript) then translation codes. transcriptLanguage picks the voice.
    SpeechDialog(Mode mode, const QString& text, const QStringList& languages, const QString& transcriptLanguage,
                 QWidget* parent = nullptr);

    QString text() const;
    QString engine() const;
    QString voice() const;
    double rate() const;
    QString language() const;          // Subtitles: "" = the transcript
    bool lowerOriginal() const;
    double originalVolume() const;

private:
    void fillVoices();

    Mode m_mode;
    QString m_transcriptLanguage;
    QPlainTextEdit* m_text = nullptr;
    QComboBox* m_language = nullptr;
    QComboBox* m_engine = nullptr;
    QComboBox* m_voice = nullptr;
    QSlider* m_rate = nullptr;
    QLabel* m_rateLabel = nullptr;
    QLabel* m_note = nullptr;
    QCheckBox* m_lower = nullptr;
    QSlider* m_origVolume = nullptr;
};

} // namespace ve
