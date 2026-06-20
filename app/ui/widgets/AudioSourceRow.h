#pragma once

#include <QWidget>

class QAbstractButton;
class QLabel;
class QSlider;

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
        bool has_gain_control = true;
        bool enabled = true;
        float gain_db = 0.0f;
        bool muted = false;
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
    void setGainControlVisible(bool visible);

    // Audio v2: gain + mute accessors.
    void setGainDb(float gain_db);
    [[nodiscard]] float gainDb() const noexcept;

    void setMuted(bool muted);
    [[nodiscard]] bool isMuted() const noexcept;

  signals:
    void sourceEnabledChanged(bool enabled);
    void mergeChanged(bool checked);
    // Audio v2
    void gainDbChanged(float gain_db);
    void mutedChanged(bool muted);

  private:
    void applyActiveState(bool active);

    VUMeterWidget* meter_ = nullptr;
    QLabel* db_label_ = nullptr;
    QWidget* merge_container_ = nullptr;
    ExoCheckBox* merge_check_ = nullptr;
    QLabel* merge_label_ = nullptr;
    ExoToggle* enabled_toggle_ = nullptr;

    // Audio v2: gain slider + mute button.
    QSlider* gain_slider_ = nullptr;
    QLabel* gain_label_ = nullptr;
    QAbstractButton* mute_button_ = nullptr;
};

} // namespace exosnap::ui::widgets
