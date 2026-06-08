#pragma once

#include <QFrame>
#include <QString>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QWidget;

namespace exosnap::ui::widgets {

class AudioSourceToggle;
class CountdownSelect;

// Stable bottom transport dock for the Record page (hybrid v3 direction).
//
// A 3-zone grid — left (source toggles / result info) | center (duration) |
// right (actions) — whose outer geometry stays constant across Ready /
// Recording / Paused / Completed. The center duration stays dead-center and the
// primary action stays in the right action area; only the contents of each zone
// show/hide between states, so the dock never reflows the page.
class TransportDock : public QFrame {
    Q_OBJECT
  public:
    enum class State { Ready, Recording, Paused, Completed };
    Q_ENUM(State)

    explicit TransportDock(QWidget* parent = nullptr);

    void setState(State state);
    [[nodiscard]] State state() const noexcept {
        return state_;
    }

    // Enables/disables the primary action(s) for the current state (e.g. a
    // blocked Ready dock keeps Record visible but disabled).
    void setPrimaryEnabled(bool enabled);

    void setTimerText(const QString& text);
    void setTimerRole(const QString& role); // idle | recording | paused | done | blocked

    // Left-zone source toggle: `on` reflects current config; `interactive`
    // controls whether the user may click it now (read-only status otherwise).
    void setToggleState(const QString& key, bool on, bool interactive);

    // Completed (result) left-zone content.
    void setCompletedInfo(const QString& filename, const QString& size_text, bool has_file);

    // Live audio meter for source toggles. key: "system" | "mic" | "app".
    // level01: 0.0 = silence/inactive, 1.0 = peak. Webcam has no audio meter.
    void setMeterLevel(const QString& key, float level01);

  signals:
    void recordClicked();
    void stopClicked();
    void pauseClicked();
    void resumeClicked();
    void recordAgainClicked();
    void openFolderClicked();
    void filenameClicked();
    void sourceToggleClicked(const QString& key);

  private:
    void applyState();

    State state_ = State::Ready;
    bool primary_enabled_ = true;

    QWidget* toggles_row_ = nullptr;
    AudioSourceToggle* system_toggle_ = nullptr;
    AudioSourceToggle* mic_toggle_ = nullptr;
    AudioSourceToggle* webcam_toggle_ = nullptr;
    AudioSourceToggle* app_toggle_ = nullptr;

    QWidget* completed_row_ = nullptr;
    QPushButton* filename_link_ = nullptr;
    QPushButton* open_folder_btn_ = nullptr;
    QLabel* size_label_ = nullptr;

    QLabel* timer_label_ = nullptr;

    QWidget* action_row_ = nullptr;
    CountdownSelect* countdown_ = nullptr;
    QPushButton* record_btn_ = nullptr;
    QPushButton* pause_btn_ = nullptr;
    QPushButton* resume_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QPushButton* record_again_btn_ = nullptr;
};

} // namespace exosnap::ui::widgets
