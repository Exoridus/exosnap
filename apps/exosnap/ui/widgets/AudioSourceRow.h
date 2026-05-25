#pragma once

#include <QWidget>

class QLabel;

namespace exosnap::ui::widgets {
class ExoCheckBox;
class ExoToggle;
class VUMeterWidget;

class AudioSourceRow : public QWidget {
    Q_OBJECT
  public:
    struct Config {
        QString tag;
        QString title;
        QString subtitle;
        QString db_value;
        bool has_merge_control = false;
        bool enabled = true;
    };

    explicit AudioSourceRow(const Config& config, QWidget* parent = nullptr);

    void setLevel(float level01);
    void setDbText(const QString& db_text);

    void setSourceEnabled(bool enabled);
    [[nodiscard]] bool isSourceEnabled() const noexcept;

    void setMergeChecked(bool checked);
    [[nodiscard]] bool mergeChecked() const noexcept;
    [[nodiscard]] bool hasMergeControl() const noexcept;

    void setMergeControlVisible(bool visible);

  signals:
    void sourceEnabledChanged(bool enabled);
    void mergeChanged(bool checked);

  private:
    void applyActiveState(bool active);

    VUMeterWidget* meter_ = nullptr;
    QLabel* db_label_ = nullptr;
    QWidget* merge_container_ = nullptr;
    ExoCheckBox* merge_check_ = nullptr;
    QLabel* merge_label_ = nullptr;
    ExoToggle* enabled_toggle_ = nullptr;
};

} // namespace exosnap::ui::widgets
