#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;

namespace ve {

// Clip > Upscale Video (AI): model (recommended one preselected, each marked with
// how it fits this computer), 2x / 4x, and whether to use the result in the clip.
class UpscaleDialog : public QDialog {
    Q_OBJECT
public:
    UpscaleDialog(int sourceWidth, int sourceHeight, int projectWidth, int projectHeight, QWidget* parent = nullptr);

    QString model() const;
    int scale() const;
    bool replaceClip() const;
    bool setProjectResolution() const;   // raise the canvas to the upscaled size

private:
    void refresh();

    int m_w, m_h, m_projectW, m_projectH;
    QComboBox* m_model = nullptr;
    QComboBox* m_scale = nullptr;
    QLabel* m_details = nullptr;
    QLabel* m_output = nullptr;
    QLabel* m_warning = nullptr;
    QPushButton* m_useRecommended = nullptr;
    QCheckBox* m_replace = nullptr;
    QCheckBox* m_projectRes = nullptr;
};

} // namespace ve
