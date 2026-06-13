#include "TransportDock.h"

#include "AudioSourceToggle.h"
#include "CountdownSelect.h"

#include <QByteArray>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QStyle>
#include <QSvgRenderer>

namespace exosnap::ui::widgets {
namespace {

QPushButton* makeActionButton(const QString& object_name, const QString& dock_action, const QString& text,
                              int min_width, QWidget* parent) {
    auto* button = new QPushButton(text, parent);
    button->setObjectName(object_name);
    button->setProperty("dockAction", dock_action);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(40);
    if (min_width > 0)
        button->setMinimumWidth(min_width);
    return button;
}

void setStyledProperty(QWidget* widget, const char* name, const QString& value) {
    if (widget->property(name).toString() == value)
        return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

// Build a 44×44 round camera icon button for the dock's capture-frame action.
// The camera glyph is the same Lucide-compatible SVG path used by AudioSourceToggle
// for the "webcam" key.  State (idle/hover/pressed/disabled) is styled via QSS on the
// "dockAction=captureFrame" property — no manual painting required.
QPushButton* makeCaptureFrameButton(QWidget* parent) {
    // Camera SVG: body + lens — identical to AudioSourceToggle "webcam" icon path.
    constexpr auto kCameraPath =
        "M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3l-2.5-3z"
        "M12 13a3 3 0 1 0 0-6 3 3 0 0 0 0 6z";

    // Build an SVG at a neutral color; QSS will tint via the object-name rule.
    QByteArray svg;
    svg.reserve(400);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none'"
               " stroke='#C8C8C4' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>"
               "<path d='");
    svg.append(kCameraPath);
    svg.append("'/></svg>");

    QSvgRenderer renderer(svg);
    constexpr int kBtn = 44;
    constexpr int kGlyph = 19;
    QPixmap pix(kGlyph, kGlyph);
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        renderer.render(&p, QRectF(0, 0, kGlyph, kGlyph));
    }

    auto* btn = new QPushButton(parent);
    btn->setObjectName(QStringLiteral("recordDockCaptureFrame"));
    btn->setProperty("dockAction", QStringLiteral("captureFrame"));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedSize(kBtn, kBtn);
    btn->setIcon(QIcon(pix));
    btn->setIconSize(QSize(kGlyph, kGlyph));
    btn->setToolTip(QStringLiteral("Capture frame (Alt+P)"));
    btn->setAccessibleName(QStringLiteral("Capture frame"));
    return btn;
}

} // namespace

TransportDock::TransportDock(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("recordTransportDock"));
    setProperty("dockState", QStringLiteral("ready"));
    setMinimumHeight(74);

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(18, 12, 18, 12);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(0);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 0);
    grid->setColumnStretch(2, 1);

    // ── LEFT zone — source toggles (default) / result info (completed) ──────
    auto* left_zone = new QWidget(this);
    auto* left_layout = new QHBoxLayout(left_zone);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(10);

    toggles_row_ = new QWidget(left_zone);
    auto* toggles_layout = new QHBoxLayout(toggles_row_);
    toggles_layout->setContentsMargins(0, 0, 0, 0);
    toggles_layout->setSpacing(10);
    system_toggle_ = new AudioSourceToggle(QStringLiteral("system"), QStringLiteral("system"), toggles_row_);
    system_toggle_->setToolTip(QStringLiteral("System audio"));
    mic_toggle_ = new AudioSourceToggle(QStringLiteral("mic"), QStringLiteral("mic"), toggles_row_);
    mic_toggle_->setToolTip(QStringLiteral("Microphone"));
    webcam_toggle_ = new AudioSourceToggle(QStringLiteral("webcam"), QStringLiteral("webcam"), toggles_row_);
    webcam_toggle_->setToolTip(QStringLiteral("Webcam"));
    app_toggle_ = new AudioSourceToggle(QStringLiteral("app"), QStringLiteral("app"), toggles_row_);
    app_toggle_->setToolTip(QStringLiteral("Application audio"));
    toggles_layout->addWidget(system_toggle_);
    toggles_layout->addWidget(mic_toggle_);
    toggles_layout->addWidget(webcam_toggle_);
    toggles_layout->addWidget(app_toggle_);

    completed_row_ = new QWidget(left_zone);
    auto* completed_layout = new QHBoxLayout(completed_row_);
    completed_layout->setContentsMargins(0, 0, 0, 0);
    completed_layout->setSpacing(10);
    filename_link_ = new QPushButton(completed_row_);
    filename_link_->setObjectName(QStringLiteral("recordDockFilename"));
    filename_link_->setCursor(Qt::PointingHandCursor);
    filename_link_->setFlat(true);
    // Icon-only 38×38 folder button (DF-04): no text label, fixed size, radius-10 via QSS.
    // Tooltip and accessible name "Open folder" are preserved for UIA/capture tooling.
    open_folder_btn_ = new QPushButton(completed_row_);
    open_folder_btn_->setObjectName(QStringLiteral("recordDockOpenFolder"));
    open_folder_btn_->setProperty("dockAction", QStringLiteral("ghost"));
    open_folder_btn_->setCursor(Qt::PointingHandCursor);
    open_folder_btn_->setToolTip(QStringLiteral("Open folder"));
    open_folder_btn_->setAccessibleName(QStringLiteral("Open folder"));
    open_folder_btn_->setIcon(QIcon(QStringLiteral(":/theme/icons/folder.svg")));
    open_folder_btn_->setIconSize(QSize(18, 18));
    open_folder_btn_->setFixedSize(38, 38);
    // D4: size_label_ is still a member for API compatibility but hidden — bar owns size.
    size_label_ = new QLabel(completed_row_);
    size_label_->setProperty("labelRole", QStringLiteral("recordDockSize"));
    size_label_->setVisible(false); // D4: size readout removed from dock; metadata bar owns it
    completed_layout->addWidget(filename_link_);
    completed_layout->addWidget(open_folder_btn_);
    // size_label_ not added to layout (D4)
    completed_row_->setVisible(false);

    left_layout->addWidget(toggles_row_);
    left_layout->addWidget(completed_row_);
    left_layout->addStretch(1);
    grid->addWidget(left_zone, 0, 0, Qt::AlignLeft | Qt::AlignVCenter);

    // ── CENTER zone — duration, always centered ─────────────────────────────
    timer_label_ = new QLabel(QStringLiteral("00:00:00"), this);
    timer_label_->setObjectName(QStringLiteral("recordDockTimer"));
    timer_label_->setProperty("labelRole", QStringLiteral("recordDockTimer"));
    timer_label_->setProperty("timerState", QStringLiteral("idle"));
    timer_label_->setAlignment(Qt::AlignCenter);
    grid->addWidget(timer_label_, 0, 1, Qt::AlignCenter);

    // ── RIGHT zone — action area ────────────────────────────────────────────
    action_row_ = new QWidget(this);
    auto* action_layout = new QHBoxLayout(action_row_);
    action_layout->setContentsMargins(0, 0, 0, 0);
    action_layout->setSpacing(10);
    countdown_ = new CountdownSelect(action_row_);
    pause_btn_ = makeActionButton(QStringLiteral("recordDockPause"), QStringLiteral("ghost"), QStringLiteral("Pause"),
                                  104, action_row_);
    resume_btn_ = makeActionButton(QStringLiteral("recordDockResume"), QStringLiteral("resume"),
                                   QStringLiteral("Resume"), 104, action_row_);
    record_btn_ = makeActionButton(QStringLiteral("recordDockRecord"), QStringLiteral("record"),
                                   QStringLiteral("Record"), 156, action_row_);
    record_again_btn_ = makeActionButton(QStringLiteral("recordDockRecordAgain"), QStringLiteral("record"),
                                         QStringLiteral("Record again"), 156, action_row_);
    stop_btn_ = makeActionButton(QStringLiteral("recordDockStop"), QStringLiteral("stop"), QStringLiteral("Stop"), 104,
                                 action_row_);
    // CAPTURE-FRAME-DOCK-BUTTON-R1: round 44×44 icon-only camera button on the right
    // side of the dock, replacing the old text "Capture frame" button and the
    // removed preview-corner overlay button.  Styled via dockAction="captureFrame".
    capture_frame_btn_ = makeCaptureFrameButton(action_row_);

    add_marker_btn_ = makeActionButton(QStringLiteral("recordDockAddMarker"), QStringLiteral("utility"),
                                       QStringLiteral("Add marker"), 0, action_row_);
    add_marker_btn_->setToolTip(QStringLiteral("Mark a notable moment on the recording timeline"));

    // Split recording — secondary (utility) action, never destructive styling.
    split_btn_ = makeActionButton(QStringLiteral("recordDockSplit"), QStringLiteral("utility"),
                                  QStringLiteral("Split recording"), 0, action_row_);
    split_btn_->setToolTip(QStringLiteral("Split recording"));

    // Fixed layout order; visibility per state keeps the right edge stable.
    action_layout->addWidget(capture_frame_btn_);
    action_layout->addWidget(add_marker_btn_);
    action_layout->addWidget(split_btn_);
    action_layout->addWidget(countdown_);
    action_layout->addWidget(pause_btn_);
    action_layout->addWidget(resume_btn_);
    action_layout->addWidget(record_btn_);
    action_layout->addWidget(record_again_btn_);
    action_layout->addWidget(stop_btn_);
    grid->addWidget(action_row_, 0, 2, Qt::AlignRight | Qt::AlignVCenter);

    connect(record_btn_, &QPushButton::clicked, this, &TransportDock::recordClicked);
    connect(stop_btn_, &QPushButton::clicked, this, &TransportDock::stopClicked);
    connect(pause_btn_, &QPushButton::clicked, this, &TransportDock::pauseClicked);
    connect(resume_btn_, &QPushButton::clicked, this, &TransportDock::resumeClicked);
    connect(record_again_btn_, &QPushButton::clicked, this, &TransportDock::recordAgainClicked);
    connect(open_folder_btn_, &QPushButton::clicked, this, &TransportDock::openFolderClicked);
    connect(filename_link_, &QPushButton::clicked, this, &TransportDock::filenameClicked);
    connect(capture_frame_btn_, &QPushButton::clicked, this, &TransportDock::captureFrameClicked);
    connect(add_marker_btn_, &QPushButton::clicked, this, &TransportDock::addMarkerClicked);
    connect(split_btn_, &QPushButton::clicked, this, &TransportDock::splitClicked);

    connect(system_toggle_, &AudioSourceToggle::clicked, this,
            [this]() { emit sourceToggleClicked(QStringLiteral("system")); });
    connect(mic_toggle_, &AudioSourceToggle::clicked, this,
            [this]() { emit sourceToggleClicked(QStringLiteral("mic")); });
    connect(webcam_toggle_, &AudioSourceToggle::clicked, this,
            [this]() { emit sourceToggleClicked(QStringLiteral("webcam")); });
    connect(app_toggle_, &AudioSourceToggle::clicked, this,
            [this]() { emit sourceToggleClicked(QStringLiteral("app")); });
    connect(countdown_, &CountdownSelect::selectedSecondsChanged, this, &TransportDock::countdownSecondsChanged);

    applyState();
}

void TransportDock::setState(State state) {
    if (state_ == state)
        return;
    state_ = state;
    applyState();
}

void TransportDock::setPrimaryEnabled(bool enabled) {
    if (primary_enabled_ == enabled)
        return;
    primary_enabled_ = enabled;
    applyState();
}

void TransportDock::applyState() {
    const bool ready = state_ == State::Ready;
    const bool countdown = state_ == State::Countdown;
    const bool recording = state_ == State::Recording;
    const bool paused = state_ == State::Paused;
    const bool saving = state_ == State::Saving;
    const bool completed = state_ == State::Completed;

    toggles_row_->setVisible(!completed && !saving);
    completed_row_->setVisible(completed || saving);

    countdown_->setVisible(ready || countdown);
    countdown_->setInteractive(ready && primary_enabled_);
    record_btn_->setVisible(ready || countdown);
    pause_btn_->setVisible(recording);
    resume_btn_->setVisible(paused);
    stop_btn_->setVisible(recording || paused);
    // In Saving state show "Record Again" disabled — so the user knows they can
    // record again once the save is done; we re-enable when saving finishes.
    record_again_btn_->setVisible(completed || saving);
    record_again_btn_->setEnabled(completed && primary_enabled_);
    capture_frame_btn_->setVisible(ready || recording || paused);
    capture_frame_btn_->setEnabled(ready || recording || paused);
    add_marker_btn_->setVisible(recording || paused);
    add_marker_btn_->setEnabled(recording || paused);
    // Split: visible only with an active session; disabled mid-transition.
    split_btn_->setVisible(recording || paused);
    split_btn_->setEnabled((recording || paused) && split_enabled_);

    record_btn_->setText(countdown ? QStringLiteral("Cancel") : QStringLiteral("Record"));
    setStyledProperty(record_btn_, "dockAction", countdown ? QStringLiteral("stop") : QStringLiteral("record"));

    record_btn_->setEnabled(primary_enabled_);
    pause_btn_->setEnabled(primary_enabled_);
    resume_btn_->setEnabled(primary_enabled_);
    stop_btn_->setEnabled(primary_enabled_);

    const char* state_name = ready       ? "ready"
                             : countdown ? "countdown"
                             : recording ? "recording"
                             : paused    ? "paused"
                             : saving    ? "saving"
                                         : "completed";
    setStyledProperty(this, "dockState", QString::fromLatin1(state_name));
}

void TransportDock::setSavingProgress(float /*fraction*/) {
    // Progress value reserved for future progress bar; currently the "Saving…"
    // label in the completed_row left zone is sufficient feedback.
    // The timer label in the center zone shows "Saving…" via setTimerText().
}

void TransportDock::setTimerText(const QString& text) {
    timer_label_->setText(text);
}

void TransportDock::setTimerRole(const QString& role) {
    setStyledProperty(timer_label_, "timerState", role);
}

int TransportDock::countdownSeconds() const {
    return countdown_ ? countdown_->selectedSeconds() : 0;
}

void TransportDock::setCountdownSeconds(int seconds) {
    if (countdown_)
        countdown_->setSelectedSeconds(seconds);
}

void TransportDock::setToggleState(const QString& key, bool on, bool interactive) {
    AudioSourceToggle* toggle = nullptr;
    if (key == QLatin1String("system"))
        toggle = system_toggle_;
    else if (key == QLatin1String("mic"))
        toggle = mic_toggle_;
    else if (key == QLatin1String("webcam"))
        toggle = webcam_toggle_;
    else if (key == QLatin1String("app"))
        toggle = app_toggle_;
    if (!toggle)
        return;
    toggle->setOn(on);
    toggle->setInteractive(interactive);
}

void TransportDock::setCompletedInfo(const QString& filename, const QString& size_text, bool has_file) {
    filename_link_->setText(filename);
    filename_link_->setToolTip(filename);
    filename_link_->setEnabled(has_file);
    open_folder_btn_->setEnabled(has_file);
    // D4: size readout removed from dock; metadata bar owns size. Store for API compat only.
    size_label_->setText(size_text);
    size_label_->setVisible(false); // always hidden
}

void TransportDock::setSplitEnabled(bool enabled) {
    if (split_enabled_ == enabled)
        return;
    split_enabled_ = enabled;
    if (split_btn_) {
        const bool active = state_ == State::Recording || state_ == State::Paused;
        split_btn_->setEnabled(active && split_enabled_);
    }
}

void TransportDock::setMeterLevel(const QString& key, float level01) {
    AudioSourceToggle* toggle = nullptr;
    if (key == QLatin1String("system"))
        toggle = system_toggle_;
    else if (key == QLatin1String("mic"))
        toggle = mic_toggle_;
    else if (key == QLatin1String("app"))
        toggle = app_toggle_;
    // "webcam" intentionally has no audio meter
    if (!toggle)
        return;
    toggle->setMeterActive(level01 > 0.0f);
    toggle->setMeterLevel(level01);
}

} // namespace exosnap::ui::widgets
