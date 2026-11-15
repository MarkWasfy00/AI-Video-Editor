#pragma once

#include <QWidget>

#include <functional>
#include <vector>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;

namespace ve {

class Project;
struct Clip;

// Property panel for the selected clip: transform, color effects, audio.
// Every change is an undoable (merged) edit.
class Inspector : public QWidget {
    Q_OBJECT
public:
    explicit Inspector(Project* project, QWidget* parent = nullptr);

public slots:
    void setClip(const QString& clipId);

private:
    // A slider + spin box bound to one double field of a Clip.
    struct Row {
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
        double scale = 1.0;                 // slider int = value * scale
        std::function<double(const Clip&)> get;
        std::function<void(Clip&, double)> set;
    };
    struct Check {
        QCheckBox* box = nullptr;
        std::function<bool(const Clip&)> get;
        std::function<void(Clip&, bool)> set;
    };

    QWidget* addRow(QFormLayout* form, const QString& label, const QString& key, double min, double max,
                    double step, int decimals, std::function<double(const Clip&)> get,
                    std::function<void(Clip&, double)> set);
    void addCheck(QFormLayout* form, const QString& label, const QString& key,
                  std::function<bool(const Clip&)> get, std::function<void(Clip&, bool)> set);
    void apply(const QString& key, const std::function<void(Clip&)>& fn);
    void reload();

    Project* m_project;
    QString m_clipId;
    QLabel* m_title = nullptr;
    QLabel* m_info = nullptr;
    QWidget* m_body = nullptr;
    QWidget* m_transformBox = nullptr;    // picture controls: video clips only
    QWidget* m_effectsBox = nullptr;
    QWidget* m_volumeRow = nullptr;       // sound controls: audio clips only
    QWidget* m_cropBox = nullptr;         // picture: video clips only
    QWidget* m_fitRow = nullptr;
    // Speed & time
    QWidget* m_speedBox = nullptr;
    QDoubleSpinBox* m_speed = nullptr;
    QComboBox* m_curve = nullptr;
    QCheckBox* m_reverse = nullptr;
    QLabel* m_timeInfo = nullptr;
    void setSpeed(double speed, const QString& curvePreset);
    void fitOrFill(bool fill);
    void rotateBy(double degrees);
    QLabel* m_linkLabel = nullptr;        // "Linked with ..." / "Not linked"
    QPushButton* m_linkButton = nullptr;  // Unlink / Link
    std::vector<Row> m_rows;
    std::vector<Check> m_checks;
    bool m_updating = false;
};

} // namespace ve
