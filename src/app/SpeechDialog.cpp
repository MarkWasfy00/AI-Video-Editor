#include "app/SpeechDialog.h"

#include "core/Project.h"
#include "media/Speech.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

namespace ve {

SpeechDialog::SpeechDialog(Mode mode, const QString& text, const QStringList& languages, const QString& transcriptLanguage,
                           QWidget* parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_transcriptLanguage(transcriptLanguage)
{
    setWindowTitle(mode == Mode::Text ? tr("Text to Speech") : tr("Voice-over from Subtitles"));
    auto* v = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    if (mode == Mode::Text) {
        m_text = new QPlainTextEdit(this);
        m_text->setPlainText(text);
        m_text->setPlaceholderText(tr("Type what the voice should say"));
        m_text->setMinimumHeight(90);
        form->addRow(tr("Text"), m_text);
    } else {
        m_language = new QComboBox(this);
        for (const QString& l : languages)
            m_language->addItem(l.isEmpty() ? tr("The transcript itself") : tr("%1 translation (dubbing)").arg(languageName(l)), l);
        form->addRow(tr("Speak"), m_language);
        connect(m_language, &QComboBox::currentIndexChanged, this, &SpeechDialog::fillVoices);
    }
    m_engine = new QComboBox(this);
    for (const QString& e : TextToSpeech::engines())
        if (TextToSpeech::engineAvailable(e)) m_engine->addItem(TextToSpeech::engineName(e), e);
    const QString saved = QSettings().value(QStringLiteral("tts/engine")).toString();
    m_engine->setCurrentIndex(std::max(0, m_engine->findData(saved)));
    m_voice = new QComboBox(this);
    m_voice->setMinimumWidth(280);
    auto* rateRow = new QHBoxLayout;
    m_rate = new QSlider(Qt::Horizontal, this);
    m_rate->setRange(50, 200);
    m_rate->setValue(QSettings().value(QStringLiteral("tts/rate"), 100).toInt());
    m_rateLabel = new QLabel(this);
    m_rateLabel->setMinimumWidth(44);
    rateRow->addWidget(m_rate, 1);
    rateRow->addWidget(m_rateLabel);
    form->addRow(tr("Engine"), m_engine);
    form->addRow(tr("Voice"), m_voice);
    form->addRow(tr("Speed"), rateRow);
    if (mode == Mode::Subtitles) {
        auto* volRow = new QHBoxLayout;
        m_lower = new QCheckBox(tr("Turn the original speech down to"), this);
        m_lower->setChecked(true);
        m_origVolume = new QSlider(Qt::Horizontal, this);
        m_origVolume->setRange(0, 100);
        m_origVolume->setValue(20);
        auto* volLabel = new QLabel(QStringLiteral("20%"), this);
        connect(m_origVolume, &QSlider::valueChanged, volLabel, [volLabel](int x) { volLabel->setText(QStringLiteral("%1%").arg(x)); });
        connect(m_lower, &QCheckBox::toggled, m_origVolume, &QWidget::setEnabled);
        volRow->addWidget(m_lower);
        volRow->addWidget(m_origVolume, 1);
        volRow->addWidget(volLabel);
        form->addRow(QString(), volRow);
    }
    v->addLayout(form);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setStyleSheet(QStringLiteral("color: #9a9aa5;"));
    v->addWidget(m_note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(mode == Mode::Text ? tr("Add Voice-over") : tr("Make Voice-over"));
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QSettings().setValue(QStringLiteral("tts/engine"), engine());
        QSettings().setValue(QStringLiteral("tts/rate"), m_rate->value());
        QSettings().setValue(QStringLiteral("tts/voice/") + engine(), voice());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_engine, &QComboBox::currentIndexChanged, this, &SpeechDialog::fillVoices);
    connect(m_rate, &QSlider::valueChanged, this, [this](int x) { m_rateLabel->setText(QStringLiteral("%1%").arg(x)); });
    m_rateLabel->setText(QStringLiteral("%1%").arg(m_rate->value()));
    fillVoices();
}

void SpeechDialog::fillVoices()
{
    const QString e = engine();
    m_voice->clear();
    // The language wanted: the subtitle language, or the transcript's.
    QString lang = m_mode == Mode::Subtitles ? language() : QString();
    if (m_mode == Mode::Subtitles && lang.isEmpty()) lang = m_transcriptLanguage;
    for (const auto& v : TextToSpeech::voices(e)) {
        QString label = v.name;
        if (!v.gender.isEmpty()) label += QStringLiteral(" - ") + v.gender;
        if (!v.installed && v.downloadMB > 0) label += tr("  (download %1 MB)").arg(v.downloadMB, 0, 'f', 0);
        m_voice->addItem(label, v.id);
    }
    QString pick = lang.isEmpty() ? QSettings().value(QStringLiteral("tts/voice/") + e).toString() : TextToSpeech::defaultVoice(e, lang);
    if (pick.isEmpty()) pick = QSettings().value(QStringLiteral("tts/voice/") + e).toString();
    m_voice->setCurrentIndex(std::max(0, m_voice->findData(pick)));
    QString note;
    if (TextToSpeech::isOnline(e))
        note = tr("Online: the text is sent to Microsoft's speech service. Has Egyptian Arabic voices (Salma, Shakir).");
    else if (e == QLatin1String("piper"))
        note = TextToSpeech::isInstalled(e) ? tr("Runs on this computer. Each voice is downloaded once (~60 MB).")
                                            : tr("Runs on this computer. The first use installs Piper (~25 MB) and the voice (~60 MB).");
    else
        note = tr("The voices installed in Windows (Settings > Time & language > Speech adds more).");
    if (m_voice->count() == 0) note += QLatin1Char(' ') + tr("No voices found for this engine.");
    m_note->setText(note);
}

QString SpeechDialog::text() const { return m_text ? m_text->toPlainText().trimmed() : QString(); }
QString SpeechDialog::engine() const { return m_engine->currentData().toString(); }
QString SpeechDialog::voice() const { return m_voice->currentData().toString(); }
double SpeechDialog::rate() const { return m_rate->value() / 100.0; }
QString SpeechDialog::language() const { return m_language ? m_language->currentData().toString() : QString(); }
bool SpeechDialog::lowerOriginal() const { return m_lower && m_lower->isChecked(); }
double SpeechDialog::originalVolume() const { return m_origVolume ? m_origVolume->value() / 100.0 : 1.0; }

} // namespace ve
