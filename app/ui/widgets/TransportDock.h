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
    // ADR-0014: Saving = MP4 remux running after recording stopped.
    enum class State { Ready, Countdown, Recording, Paused, Saving, Completed };
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
    void setTimerRole(const QString& role); // idle | countdown | recording | paused | done | blocked

    [[nodiscard]] int countdownSeconds() const;
    void setCountdownSeconds(int seconds);

    // Left-zone source toggle: `on` reflects current config; `interactive`
    // controls whether the user may click it now (read-only status otherwise).
    void setToggleState(const QString& key, bool on, bool interactive);

    // Completed (result) left-zone content.
    void setCompletedInfo(const QString& filename, const QString& size_text, bool has_file);

    // Live audio meter for source toggles. key: "system" | "mic" | "app".
    // level01: 0.0 = silence/inactive, 1.0 = peak. Webcam has no audio meter.
    void setMeterLevel(const QString& key, float level01);

    // Split action gating (SPLIT-RECORDING-R1). The button is only visible in
    // Recording/Paused; this disables it briefly while a split transition is in
    // flight so coalesced clicks do not pile up.
    void setSplitEnabled(bool enabled);

    // ADR-0014: update remux progress displayed in the Saving state.
    // fraction in [0, 1]; pass -1 for indeterminate (spinner/pulse).
    void setSavingProgress(float fraction);

  signals:
    void recordClicked();
    void stopClicked();
    void pauseClicked();
    void resumeClicked();
    void recordAgainClicked();
    void openFolderClicked();
    void filenameClicked();
    void captureFrameClicked();
    void addMarkerClicked();
    void splitClicked();
    void sourceToggleClicked(const QString& key);
    void countdownSecondsChanged(int seconds);

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
    QPushButton* capture_frame_btn_ = nullptr;
    QPushButton* add_marker_btn_ = nullptr;
    QPushButton* split_btn_ = nullptr;
    bool split_enabled_ = true;
};

} // namespace exosnap::ui::widgets
