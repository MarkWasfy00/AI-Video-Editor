#pragma once

#include "media/Exporter.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace ve {

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(const ProjectSettings& project, const QString& suggestedPath, QWidget* parent = nullptr);
    ExportSettings settings() const;

private:
    void browse();
    void updateExtension();

    ProjectSettings m_project;
    QLineEdit* m_path = nullptr;
    QComboBox* m_resolution = nullptr;
    QDoubleSpinBox* m_fps = nullptr;
    QComboBox* m_encoder = nullptr;
    QSpinBox* m_crf = nullptr;
    QCheckBox* m_audio = nullptr;
};

} // namespace ve
