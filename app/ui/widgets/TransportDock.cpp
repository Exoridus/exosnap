#include "TransportDock.h"

#include "AudioSourceToggle.h"

#include "../theme/ExoSnapTheme.h"

#include <QAction>
#include <QByteArray>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>

namespace exosnap::ui::widgets {
namespace {

// Glyph rendered ahead of a transport-button label. The design system shows each
// transport action as [thin/hollow glyph] + [label]; the SVG bodies below are all
// fill:none / round-stroke so they read as crisp outlines on the button fill.
struct TransportGlyph {
    const char* body; // SVG element(s) inside a 0 0 24 24 viewBox (no <svg> wrapper)
};

// Hollow circle — record / record-again (mint button, accent-ink glyph).
constexpr TransportGlyph kRecordGlyph{"<circle cx='12' cy='12' r='6.5'/>"};
// Two slim vertical bars — pause (ghost button, secondary-text glyph).
constexpr TransportGlyph kPauseGlyph{"<line x1='9' y1='7.5' x2='9' y2='16.5'/>"
                                     "<line x1='15' y1='7.5' x2='15' y2='16.5'/>"};
// Hollow rounded square — stop (error button, dark glyph).
constexpr TransportGlyph kStopGlyph{"<rect x='6.5' y='6.5' width='11' height='11' rx='2' ry='2'/>"};
// Hollow play triangle — resume (mint button, accent-ink glyph).
constexpr TransportGlyph kResumeGlyph{"<path d='M9 7.5 L17 12 L9 16.5 Z'/>"};

// Build a transparent QPixmap of the given glyph stroked in `color`, matching the
// makeCaptureFrameButton recipe (fill none, round caps/joins, ~1.7 stroke). Baked
// at construction; TransportDock has no theme-refresh hook so the colours are
// captured from the active theme once, like the capture-frame button.
QPixmap renderGlyph(const TransportGlyph& glyph, const QString& color, int glyph_px) {
    QByteArray svg;
    svg.reserve(256);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(color.toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'>");
    svg.append(glyph.body);
    svg.append("</svg>");

    QSvgRenderer renderer(svg);
    QPixmap pix(glyph_px, glyph_px);
    pix.fill(Qt::transparent);
    {
        QPainter p(&pix);
        renderer.render(&p, QRectF(0, 0, glyph_px, glyph_px));
    }
    return pix;
}

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

// Attach a thin/hollow transport glyph (icon-left, before the existing text label)
// to a transport button. `color` must contrast the button's QSS background.
void setTransportGlyph(QPushButton* button, const TransportGlyph& glyph, const QString& color) {
    constexpr int kGlyphPx = 17; // ~glyph size matched to the ~13–14px button label
    button->setIcon(QIcon(renderGlyph(glyph, color, kGlyphPx)));
    button->setIconSize(QSize(kGlyphPx, kGlyphPx));
}

void setStyledProperty(QWidget* widget, const char* name, const QString& value) {
    if (widget->property(name).toString() == value)
        return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

// Build a 44×44 round icon-only button for a dock action (flag / scissors / etc.).
// The SVG path is rendered at 18px into a transparent pixmap; QSS on dockAction
// drives the background/border (same treatment as the capture-frame button).
QPushButton* makeIconActionButton(const QString& object_name, const QString& dock_action, const char* svg_path_d,
                                  const QString& tooltip, QWidget* parent) {
    // DESIGN-FIDELITY: suite-record.jsx:87 IconActionBtn — glyph is 19px in HT.mut
    // (was a brighter hard-coded #C8C8C4 at 18px).
    const QString icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut);
    QByteArray svg;
    svg.reserve(400);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(icon_color.toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
    svg.append(svg_path_d);
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
    btn->setObjectName(object_name);
    btn->setProperty("dockAction", dock_action);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFixedSize(kBtn, kBtn);
    btn->setIcon(QIcon(pix));
    btn->setIconSize(QSize(kGlyph, kGlyph));
    btn->setToolTip(tooltip);
    btn->setAccessibleName(tooltip);
    return btn;
}

// Build a 44×44 round image icon button for the dock's capture-frame action.
// Uses the Lucide "image" path (landscape photo framing) to distinguish it from
// the webcam toggle which uses the camera icon.
// State (idle/hover/pressed/disabled) is styled via QSS on the
// "dockAction=captureFrame" property — no manual painting required.
QPushButton* makeCaptureFrameButton(QWidget* parent) {
    // Lucide "image" icon — rectangular frame with mountain + sun, distinct from the
    // camera glyph used by AudioSourceToggle for the "webcam" key.
    constexpr auto kCameraPath = "M21 15a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7a2 2 0 0 1 2-2h4l2 3h8a2 2 0 0 1 2 2z";

    // DESIGN-FIDELITY: suite-record.jsx:39 CapDockBtn — camera glyph 19px in HT.mut.
    const QString icon_color = QString::fromUtf8(exosnap::ui::theme::ActiveTheme().mut);
    QByteArray svg;
    svg.reserve(400);
    svg.append("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='");
    svg.append(icon_color.toUtf8());
    svg.append("' stroke-width='1.7' stroke-linecap='round' stroke-linejoin='round'><path d='");
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
    // DESIGN-FIDELITY: State-Spec "Dock-Container" min-height = 72px.
    setMinimumHeight(72);

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
    toggles_layout->setSpacing(9); // State-Spec: Source-Toggle gap 9px
    system_toggle_ = new AudioSourceToggle(QStringLiteral("system"), QStringLiteral("system"), toggles_row_);
    system_toggle_->setToolTip(QStringLiteral("System audio"));
    mic_toggle_ = new AudioSourceToggle(QStringLiteral("mic"), QStringLiteral("mic"), toggles_row_);
    mic_toggle_->setToolTip(QStringLiteral("Microphone"));
    webcam_toggle_ = new AudioSourceToggle(QStringLiteral("webcam"), QStringLiteral("webcam"), toggles_row_);
    webcam_toggle_->setToolTip(QStringLiteral("Webcam"));
    app_toggle_ = new AudioSourceToggle(QStringLiteral("app"), QStringLiteral("app"), toggles_row_);
    app_toggle_->setToolTip(QStringLiteral("App audio"));
    // DESIGN-FIDELITY: toggle order is speaker → app → mic → webcam, per the
    // newest Mappe (suite-record.jsx:164-167 / :264-267, text "SYS → APP → MIC → CAM").
    toggles_layout->addWidget(system_toggle_); // speaker (System audio)
    toggles_layout->addWidget(app_toggle_);    // app
    toggles_layout->addWidget(mic_toggle_);    // mic
    toggles_layout->addWidget(webcam_toggle_); // webcam

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
    action_layout->setSpacing(9); // State-Spec: right action cluster gap 9px

    // v10 split Record button: pill-shaped container with a main "Record" face
    // and a chevron face that opens the countdown menu (3 / 5 / 10 s).
    // Container clips both parts to a single pill via QSS overflow:hidden + radius.
    record_split_container_ = new QFrame(action_row_);
    record_split_container_->setObjectName(QStringLiteral("recordSplitContainer"));
    {
        auto* split_layout = new QHBoxLayout(record_split_container_);
        split_layout->setContentsMargins(0, 0, 0, 0);
        split_layout->setSpacing(0);

        record_btn_ = makeActionButton(QStringLiteral("recordDockRecord"), QStringLiteral("record"),
                                       QStringLiteral("Record"), 0, record_split_container_);
        record_btn_->setMinimumWidth(116);
        record_btn_->setMinimumHeight(46);
        // Remove individual border-radius so the container clips both halves.
        record_btn_->setProperty("splitRole", QStringLiteral("face"));

        // Thin vertical separator between the two halves — styled via QSS.
        auto* divider = new QFrame(record_split_container_);
        divider->setObjectName(QStringLiteral("recordSplitDivider"));
        divider->setFrameShape(QFrame::VLine);
        divider->setFixedWidth(1);

        // Chevron half — opens the countdown QMenu above the dock.
        record_chevron_btn_ = new QPushButton(record_split_container_);
        record_chevron_btn_->setObjectName(QStringLiteral("recordDockChevron"));
        record_chevron_btn_->setProperty("dockAction", QStringLiteral("record"));
        record_chevron_btn_->setProperty("splitRole", QStringLiteral("chevron"));
        record_chevron_btn_->setCursor(Qt::PointingHandCursor);
        record_chevron_btn_->setFixedWidth(40);
        record_chevron_btn_->setMinimumHeight(46);
        record_chevron_btn_->setToolTip(QStringLiteral("Start with a countdown"));
        record_chevron_btn_->setAccessibleName(QStringLiteral("Countdown options"));

        // Render a small chevron-down glyph for the button icon.
        {
            constexpr auto kChevronBody = "<polyline points='6 9 12 15 18 9'/>";
            constexpr int kPx = 15;
            const auto& th = exosnap::ui::theme::ActiveTheme();
            const QString ink = QString::fromUtf8(th.ac_ink);
            QPixmap pix = renderGlyph(TransportGlyph{kChevronBody}, ink, kPx);
            record_chevron_btn_->setIcon(QIcon(pix));
            record_chevron_btn_->setIconSize(QSize(kPx, kPx));
        }

        split_layout->addWidget(record_btn_);
        split_layout->addWidget(divider);
        split_layout->addWidget(record_chevron_btn_);
    }

    pause_btn_ = makeActionButton(QStringLiteral("recordDockPause"), QStringLiteral("ghost"), QStringLiteral("Pause"),
                                  104, action_row_);
    resume_btn_ = makeActionButton(QStringLiteral("recordDockResume"), QStringLiteral("resume"),
                                   QStringLiteral("Resume"), 104, action_row_);
    record_again_btn_ = makeActionButton(QStringLiteral("recordDockRecordAgain"), QStringLiteral("record"),
                                         QStringLiteral("Record again"), 156, action_row_);
    stop_btn_ = makeActionButton(QStringLiteral("recordDockStop"), QStringLiteral("stop"), QStringLiteral("Stop"), 104,
                                 action_row_);

    // DESIGN-SYNC-R1: thin/hollow transport glyphs ahead of each label. Colours are
    // baked from the active theme to contrast each button's QSS fill:
    //   record / resume / record-again → mint fill → accent-ink (dark) glyph
    //   stop                           → error/coral fill → #1A0D0B (matches QSS text)
    //   pause                          → ghost/transparent → secondary text (text2 = mut)
    // NOTE: record_btn_ glyph already set above (chevron icon baked at same time).
    // Re-bake record glyph here so it aligns with the other transport glyphs.
    const auto& glyph_theme = exosnap::ui::theme::ActiveTheme();
    const QString accent_ink = QString::fromUtf8(glyph_theme.ac_ink);
    const QString ghost_text = QString::fromUtf8(glyph_theme.mut); // ${text2}
    const QString stop_ink = QStringLiteral("#1A0D0B");            // matches dockAction="stop" text
    setTransportGlyph(record_btn_, kRecordGlyph, accent_ink);
    setTransportGlyph(record_again_btn_, kRecordGlyph, accent_ink);
    setTransportGlyph(resume_btn_, kResumeGlyph, accent_ink);
    setTransportGlyph(pause_btn_, kPauseGlyph, ghost_text);
    setTransportGlyph(stop_btn_, kStopGlyph, stop_ink);

    // CAPTURE-FRAME-DOCK-BUTTON-R1: round 44×44 icon-only camera button on the right
    // side of the dock, replacing the old text "Capture frame" button and the
    // removed preview-corner overlay button.  Styled via dockAction="captureFrame".
    capture_frame_btn_ = makeCaptureFrameButton(action_row_);

    // v10: Add marker → icon-only 44×44 round button (flag icon), "iconAction" dockAction.
    // Lucide flag: M4 15s3-5 0-9.5C5.8 4.8 9 3 12 3s6.2 1.8 7.5 6c-3.3 4-5 7-7.5 7s-4.2-3-8-1z
    // Simplified straight-flag path (clean at small sizes): M4 15 L4 3 L15 7 L4 11
    constexpr auto kFlagPath = "M4 15V3l11 4L4 11"
                               "M4 3v18"; // vertical pole
    add_marker_btn_ = makeIconActionButton(QStringLiteral("recordDockAddMarker"), QStringLiteral("iconAction"),
                                           kFlagPath, QStringLiteral("Add marker"), action_row_);

    // v10: Split → icon-only 44×44 round button (scissors icon), "iconAction" dockAction.
    // Lucide scissors path:
    constexpr auto kScissorsPath = "M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6z"
                                   "M6 15a3 3 0 1 0 0 6 3 3 0 0 0 0-6z"
                                   "M20 4L8.12 15.88"
                                   "M14.47 14.48L20 20"
                                   "M8.12 8.12L12 12";
    split_btn_ = makeIconActionButton(QStringLiteral("recordDockSplit"), QStringLiteral("iconAction"), kScissorsPath,
                                      QStringLiteral("Split recording"), action_row_);

    // Fixed layout order; visibility per state keeps the right edge stable.
    // record_btn_ is a child of record_split_container_, not added directly here.
    action_layout->addWidget(capture_frame_btn_);
    action_layout->addWidget(add_marker_btn_);
    action_layout->addWidget(split_btn_);
    action_layout->addWidget(pause_btn_);
    action_layout->addWidget(resume_btn_);
    action_layout->addWidget(record_split_container_);
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

    // Chevron button: hover-triggered countdown menu (v10 spec).
    // The menu opens when the cursor enters the chevron; a short leave-delay
    // prevents accidental dismiss when the user moves toward the menu items.
    // Clicking a menu item starts the recording with the chosen countdown delay.
    chevron_leave_timer_ = new QTimer(this);
    chevron_leave_timer_->setSingleShot(true);
    chevron_leave_timer_->setInterval(300); // ms — generous enough for mouse travel
    connect(chevron_leave_timer_, &QTimer::timeout, this, [this]() {
        if (chevron_menu_ && !chevron_menu_->underMouse()) {
            chevron_menu_->close();
        }
    });
    record_chevron_btn_->installEventFilter(this);

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
    // Saving and Completed are kept in the enum for API compatibility but the
    // dock now treats them both as Ready (v10: no separate saved/saving panel).
    const bool saving = state_ == State::Saving;
    const bool completed = state_ == State::Completed;
    const bool show_ready = ready || saving || completed;

    // v10: left zone always shows the source toggles (no completed_row_ in active layout).
    toggles_row_->setVisible(true);
    completed_row_->setVisible(false);

    // The split container (record face + chevron) is shown in Ready/Saving/Completed
    // and Countdown states. In Countdown the record face becomes "Cancel"
    // (stop-styled) and the chevron is disabled so the user cannot change the delay.
    record_split_container_->setVisible(show_ready || countdown);
    record_chevron_btn_->setEnabled(show_ready && primary_enabled_);
    pause_btn_->setVisible(recording);
    resume_btn_->setVisible(paused);
    stop_btn_->setVisible(recording || paused);
    // v10: record_again_btn_ never shown (completed state → Ready, not a distinct layout).
    record_again_btn_->setVisible(false);
    record_again_btn_->setEnabled(false);
    // Capture-frame: shown in ready/recording/paused; disabled while saving/blocked.
    capture_frame_btn_->setVisible(show_ready || recording || paused);
    capture_frame_btn_->setEnabled((ready || recording || paused) && primary_enabled_);
    add_marker_btn_->setVisible(recording || paused);
    add_marker_btn_->setEnabled(recording || paused);
    // Split: visible only with an active session; disabled mid-transition.
    split_btn_->setVisible(recording || paused);
    split_btn_->setEnabled((recording || paused) && split_enabled_);

    // In Countdown state the Record face becomes "Cancel" (stop-styled container);
    // also propagate the dockAction to the container so QSS can restyle the pill.
    record_btn_->setText(countdown ? QStringLiteral("Cancel") : QStringLiteral("Record"));
    setStyledProperty(record_btn_, "dockAction", countdown ? QStringLiteral("stop") : QStringLiteral("record"));
    setStyledProperty(record_split_container_, "containerAction",
                      countdown ? QStringLiteral("stop") : QStringLiteral("record"));

    record_btn_->setEnabled(primary_enabled_);
    pause_btn_->setEnabled(primary_enabled_);
    resume_btn_->setEnabled(primary_enabled_);
    stop_btn_->setEnabled(primary_enabled_);

    const char* state_name = show_ready ? "ready" : countdown ? "countdown" : recording ? "recording" : "paused";
    setStyledProperty(this, "dockState", QString::fromLatin1(state_name));
}

bool TransportDock::eventFilter(QObject* watched, QEvent* event) {
    if (watched == record_chevron_btn_) {
        if (event->type() == QEvent::Enter) {
            // Stop any pending close and open the menu if enabled and not already open.
            chevron_leave_timer_->stop();
            if (record_chevron_btn_->isEnabled() && (!chevron_menu_ || !chevron_menu_->isVisible())) {
                openChevronMenu();
            }
        } else if (event->type() == QEvent::Leave) {
            // Delay close so the cursor has time to travel into the menu.
            chevron_leave_timer_->start();
        }
    }
    return QFrame::eventFilter(watched, event);
}

void TransportDock::openChevronMenu() {
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("recordCountdownMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    chevron_menu_ = menu;
    connect(menu, &QObject::destroyed, this, [this]() { chevron_menu_ = nullptr; });

    // Stop the leave-timer when the mouse enters the menu so it does not close.
    connect(menu, &QMenu::aboutToHide, this, [this]() { chevron_leave_timer_->stop(); });

    struct {
        const char* label;
        int seconds;
    } delays[] = {
        {"Start in 3 seconds", 3},
        {"Start in 5 seconds", 5},
        {"Start in 10 seconds", 10},
    };
    for (const auto& d : delays) {
        QAction* action = menu->addAction(QString::fromLatin1(d.label));
        action->setObjectName(QStringLiteral("recordCountdownAction_%1s").arg(d.seconds));
        action->setProperty("countdownSeconds", d.seconds);
        connect(action, &QAction::triggered, this, [this, seconds = d.seconds]() {
            selected_countdown_seconds_ = seconds;
            emit countdownSecondsChanged(seconds);
        });
    }

    // Pop up above the chevron button.
    const QPoint pos = record_chevron_btn_->mapToGlobal(QPoint(0, 0));
    menu->popup(QPoint(pos.x(), pos.y() - menu->sizeHint().height() - 4));
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
    return selected_countdown_seconds_;
}

void TransportDock::setCountdownSeconds(int seconds) {
    // Callers (RecordPage::setCountdownSeconds) already snap to {0,3,5,10}.
    // Store verbatim — the dock is a display/round-trip store, not the policy owner.
    selected_countdown_seconds_ = seconds;
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

void TransportDock::setToggleVisible(const QString& key, bool visible) {
    AudioSourceToggle* toggle = nullptr;
    if (key == QLatin1String("system"))
        toggle = system_toggle_;
    else if (key == QLatin1String("mic"))
        toggle = mic_toggle_;
    else if (key == QLatin1String("webcam"))
        toggle = webcam_toggle_;
    else if (key == QLatin1String("app"))
        toggle = app_toggle_;
    if (toggle)
        toggle->setVisible(visible);
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
