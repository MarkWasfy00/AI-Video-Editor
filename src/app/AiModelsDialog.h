#pragma once

#include "media/AiModels.h"

#include <QDialog>
#include <QMap>

class QComboBox;
class QLabel;

namespace ve {

// Tools > AI Models: this computer's hardware, and for each AI task the models
// with what they need, how they fit here and which one is recommended.
// Startup mode (limited hardware): explains why and preselects the recommendations.
class AiModelsDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Settings, Startup };
    explicit AiModelsDialog(Mode mode, QWidget* parent = nullptr);

    // Saves the chosen models.
    void apply();

    // On start-up: if this computer cannot run the best models well and the user has
    // not been asked for this hardware yet, offers the recommended models.
    static void promptIfLimited(QWidget* parent);

private:
    void updateDetails(AiTask task);
    void useRecommended();
    void refill();                       // after the "downloaded only" filter changes

    Mode m_mode;
    QMap<AiTask, QComboBox*> m_combos;
    QMap<AiTask, QLabel*> m_details;
    bool m_downloadedOnly = false;
};

// Combo box entries "<name> - <fit>" for the models of a task, the recommended one
// marked; item data = model id. Shared with the upscale dialog.
// downloadedOnly: only installed models (plus `current`, so the choice stays visible).
void fillModelCombo(QComboBox* combo, AiTask task, const QString& current, bool downloadedOnly = false);
// Rich text with the description, requirements and fit of a model.
QString modelDetailsHtml(const QString& modelId);

} // namespace ve
