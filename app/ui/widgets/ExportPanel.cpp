#include "ExportPanel.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

#include <QComboBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

using M = exosnap::ui::theme::ExoSnapMetrics;
using exosnap::ui::theme::ActiveTheme;

namespace {

QString tok(const char* base) {
    return QString::fromUtf8(base);
}

// Derived alpha tokens for the result badge (mirrors BuildTokens() in
// ExoSnapTheme.cpp).
QString dimToken(const char* base_css) {
    const auto& t = ActiveTheme();
    return theme::ThemeRgba(theme::ParseThemeColor(base_css), t.kind == theme::ThemeKind::Dark ? 0.13 : 0.12);
}
QString borderToken(const char* base_css) {
    const auto& t = ActiveTheme();
    return theme::ThemeRgba(theme::ParseThemeColor(base_css), t.kind == theme::ThemeKind::Dark ? 0.44 : 0.42);
}

const QString kEmptyDetail = QStringLiteral("\xe2\x80\x94");

// Vertical rhythm of the result group. The follow-up actions are stacked (a
// 240 px rail cannot hold them side by side), so their own height and the gap
// between them decide how much taller Done is than the other three states.
constexpr int kResultSpacing = 6;
constexpr int kResultButtonHeight = 32;

// A combo in a 240-320 px rail must not demand the width of its longest item:
// its size hint would push the whole column wider than the breakpoint allows.
void MakeRailCombo(QComboBox* combo) {
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(4);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

} // namespace

ExportPanel::ExportPanel(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("exportPanel"));
    buildPanel();
    refreshDestinationText();
    refreshStateVisibility();
    applyThemeStyles();
}

void ExportPanel::buildPanel() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    root->setSpacing(M::kSpaceSm);

    title_label_ = new QLabel(QStringLiteral("Export"), this);
    title_label_->setObjectName(QStringLiteral("exportPanelTitle"));
    root->addWidget(title_label_);

    // ---- Status area: directly under the title, above the settings ----
    // Progress and result belong where the eye already is when the export
    // starts. Below the settings they were past the fold at the minimum window
    // height, and the only way to show them was to scroll the whole rail — which
    // moved the details card and the card heading out from under the pointer for
    // every state change. Here nothing above them moves, and the settings are
    // what scrolls away instead.
    buildStatusArea(root);

    // ---- Output rows: container + save mode + what the destination will be ----
    options_content_ = new QWidget(this);
    options_content_->setObjectName(QStringLiteral("exportPanelOptions"));
    auto* options_layout = new QVBoxLayout(options_content_);
    options_layout->setContentsMargins(0, 0, 0, 0);
    options_layout->setSpacing(M::kSpaceXs);

    container_label_ = new QLabel(QStringLiteral("Container"), options_content_);
    options_layout->addWidget(container_label_);

    // Both containers are stream-copy and lossless; saying so once in the
    // destination line below beats repeating it inside each item, which in a
    // rail-width combo would only be clipped.
    container_combo_ = new QComboBox(options_content_);
    container_combo_->setObjectName(QStringLiteral("outputContainerCombo"));
    container_combo_->addItem(QStringLiteral("MKV"), QStringLiteral("mkv"));
    container_combo_->addItem(QStringLiteral("MP4"), QStringLiteral("mp4"));
    MakeRailCombo(container_combo_);
    options_layout->addWidget(container_combo_);

    options_layout->addSpacing(M::kSpaceXs);

    save_mode_label_ = new QLabel(QStringLiteral("Save"), options_content_);
    options_layout->addWidget(save_mode_label_);

    save_mode_combo_ = new QComboBox(options_content_);
    save_mode_combo_->setObjectName(QStringLiteral("outputSaveModeCombo"));
    save_mode_combo_->addItem(QStringLiteral("New file"), QStringLiteral("new"));
    save_mode_combo_->addItem(QStringLiteral("Overwrite original"), QStringLiteral("overwrite"));
    MakeRailCombo(save_mode_combo_);
    options_layout->addWidget(save_mode_combo_);

    options_layout->addSpacing(M::kSpaceXs);

    // Destination is fully determined by the save mode above — no folder picker
    // (ADR 0022) — so this line only states what the choice means.
    dest_label_ = new QLabel(options_content_);
    dest_label_->setObjectName(QStringLiteral("editExportDestFolder"));
    dest_label_->setWordWrap(true);
    {
        // setWordWrap() alone leaves a one-line minimum height, which clips the
        // wrapped sentence inside the rail's fixed width. Ignored horizontally
        // on top of that: the rail is fixed-width, so the label has to take
        // whatever it is given rather than report a width the column must honour.
        QSizePolicy policy = dest_label_->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        policy.setHeightForWidth(true);
        dest_label_->setSizePolicy(policy);
    }
    options_layout->addWidget(dest_label_);

    root->addWidget(options_content_);

    connect(save_mode_combo_, &QComboBox::currentIndexChanged, this, [this](int) { refreshDestinationText(); });
    connect(container_combo_, &QComboBox::currentIndexChanged, this, [this](int) { refreshDestinationText(); });

    // ---- Wiring: buttons only ever emit — the page owns every transition ----
    connect(cancel_btn_, &QPushButton::clicked, this, &ExportPanel::cancelRequested);
    connect(retry_btn_, &QPushButton::clicked, this, &ExportPanel::retryRequested);
    connect(open_folder_btn_, &QPushButton::clicked, this, &ExportPanel::openFolderRequested);
    connect(reveal_btn_, &QPushButton::clicked, this, &ExportPanel::revealFileRequested);
}

// Hidden until a run has something to report. A QBoxLayout skips hidden items
// and the spacing around them, so the resting card reserves no empty band here.
void ExportPanel::buildStatusArea(QVBoxLayout* root) {
    running_content_ = new QWidget(this);
    running_content_->setObjectName(QStringLiteral("exportPanelRunning"));
    auto* running_layout = new QVBoxLayout(running_content_);
    running_layout->setContentsMargins(0, 0, 0, 0);
    running_layout->setSpacing(M::kSpaceSm);

    status_label_ = new QLabel(QStringLiteral("Exporting\xe2\x80\xa6"), running_content_);
    status_label_->setObjectName(QStringLiteral("exportStatusLabel"));

    progress_bar_ = new QProgressBar(running_content_);
    progress_bar_->setObjectName(QStringLiteral("exportProgressBar"));
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setFixedHeight(6);
    progress_bar_->setTextVisible(false);

    cancel_btn_ = new QPushButton(QStringLiteral("Cancel"), running_content_);
    cancel_btn_->setObjectName(QStringLiteral("exportCancelBtn"));
    cancel_btn_->setProperty("role", "ghost");

    running_layout->addWidget(status_label_);
    running_layout->addWidget(progress_bar_);
    running_layout->addWidget(cancel_btn_);

    root->addWidget(running_content_);

    // ---- Result: Done and Failed share the same shape ----
    result_content_ = new QWidget(this);
    result_content_->setObjectName(QStringLiteral("exportPanelResult"));
    auto* result_layout = new QVBoxLayout(result_content_);
    result_layout->setContentsMargins(0, 0, 0, 0);
    // Tighter than the running group's rhythm: this one carries two stacked
    // full-width buttons, and at the standard gap the Done state stood visibly
    // taller than Running and Failed for no reason the user can act on.
    result_layout->setSpacing(kResultSpacing);

    // Badge and headline on one line: a 56 px circular badge (the card version's
    // shape) would eat a quarter of the rail's width on its own.
    auto* result_head = new QWidget(result_content_);
    auto* result_head_layout = new QHBoxLayout(result_head);
    result_head_layout->setContentsMargins(0, 0, 0, 0);
    result_head_layout->setSpacing(M::kSpaceSm);

    result_icon_label_ = new QLabel(result_head);
    result_icon_label_->setObjectName(QStringLiteral("exportResultIcon"));
    result_icon_label_->setFixedSize(28, 28);
    result_icon_label_->setAlignment(Qt::AlignCenter);

    result_title_label_ = new QLabel(result_head);
    result_title_label_->setObjectName(QStringLiteral("exportResultTitle"));
    result_title_label_->setWordWrap(true);

    result_head_layout->addWidget(result_icon_label_);
    result_head_layout->addWidget(result_title_label_, 1);

    result_detail_label_ = new QLabel(result_content_);
    result_detail_label_->setObjectName(QStringLiteral("exportResultDetail"));
    result_detail_label_->setWordWrap(true);
    {
        // Same reason as the destination line, plus one of its own: a remuxer
        // error can carry an unbreakable token (a path), and a label allowed to
        // demand that width would push the rail's content past the column.
        QSizePolicy policy = result_detail_label_->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        policy.setHeightForWidth(true);
        result_detail_label_->setSizePolicy(policy);
    }

    // Stacked, not side by side: "Open folder" and "Show in Explorer" next to
    // each other do not fit the narrow rail without eliding one of them away.
    // The height is fixed rather than left to the control metric — stacked in a
    // column these are secondary follow-ups, not two full-size form controls,
    // and setFixedHeight keeps that identical under every platform style.
    open_folder_btn_ = new QPushButton(QStringLiteral("Open folder"), result_content_);
    open_folder_btn_->setObjectName(QStringLiteral("exportOpenFolderBtn"));
    open_folder_btn_->setProperty("role", "ghost");
    open_folder_btn_->setFixedHeight(kResultButtonHeight);

    reveal_btn_ = new QPushButton(QStringLiteral("Show in Explorer"), result_content_);
    reveal_btn_->setObjectName(QStringLiteral("exportRevealBtn"));
    reveal_btn_->setProperty("role", "ghost");
    reveal_btn_->setFixedHeight(kResultButtonHeight);

    retry_btn_ = new QPushButton(QStringLiteral("Retry"), result_content_);
    retry_btn_->setObjectName(QStringLiteral("exportRetryBtn"));
    retry_btn_->setProperty("role", "ghost");
    retry_btn_->setFixedHeight(kResultButtonHeight);

    result_layout->addWidget(result_head);
    result_layout->addWidget(result_detail_label_);
    result_layout->addWidget(open_folder_btn_);
    result_layout->addWidget(reveal_btn_);
    result_layout->addWidget(retry_btn_);

    root->addWidget(result_content_);

    // Divides the run's status from the settings below it, and is shown with
    // them: in Options there is nothing above the line to separate.
    status_separator_ = new QFrame(this);
    status_separator_->setObjectName(QStringLiteral("exportPanelSeparator"));
    status_separator_->setFixedHeight(1);
    root->addWidget(status_separator_);
}

ExportPanel::State ExportPanel::state() const noexcept {
    return state_;
}

QString ExportPanel::containerKey() const {
    return container_combo_->currentData().toString();
}

QString ExportPanel::saveModeKey() const {
    return save_mode_combo_->currentData().toString();
}

void ExportPanel::reset() {
    state_ = State::Options;
    progress_bar_->setValue(0);
    refreshStateVisibility();
}

void ExportPanel::showRunning() {
    state_ = State::Running;
    status_label_->setText(QStringLiteral("Exporting\xe2\x80\xa6"));
    progress_bar_->setValue(0);
    refreshStateVisibility();
}

void ExportPanel::setProgress(int percent) {
    progress_bar_->setValue(qBound(0, percent, 100));
}

void ExportPanel::showDone(const QString& output_path) {
    state_ = State::Done;
    QString file_name = QFileInfo(output_path).fileName();
    if (file_name.isEmpty())
        file_name = kEmptyDetail;
    result_title_label_->setText(QStringLiteral("Export complete"));
    result_detail_label_->setText(file_name);
    refreshStateVisibility();
    applyThemeStyles(); // re-derive the badge/title colours for the new state
}

void ExportPanel::showFailed(const QString& error_message) {
    state_ = State::Failed;
    result_title_label_->setText(QStringLiteral("Export failed"));
    result_detail_label_->setText(error_message.isEmpty() ? QStringLiteral("Unknown error") : error_message);
    refreshStateVisibility();
    applyThemeStyles(); // re-derive the badge/title colours for the new state
}

void ExportPanel::refreshStateVisibility() {
    const bool show_running = (state_ == State::Running);
    const bool show_result = (state_ == State::Done || state_ == State::Failed);

    // The output rows never disappear — they are the panel's resting content —
    // but a run in flight must not have its container or target changed under it.
    options_content_->setEnabled(!show_running);

    status_separator_->setVisible(show_running || show_result);
    running_content_->setVisible(show_running);
    result_content_->setVisible(show_result);

    open_folder_btn_->setVisible(state_ == State::Done);
    reveal_btn_->setVisible(state_ == State::Done);
    retry_btn_->setVisible(state_ == State::Failed);
}

void ExportPanel::refreshDestinationText() {
    const bool overwrite = saveModeKey() == QStringLiteral("overwrite");
    const QString ext = containerKey() == QStringLiteral("mp4") ? QStringLiteral("mp4") : QStringLiteral("mkv");
    // Two short lines rather than one running sentence: this is the consequence
    // of the save-mode choice, and in the rail a flowing sentence was the one
    // thing that ran out of column. The lead line is the same in both modes, so
    // switching modes changes only the line that actually differs.
    dest_label_->setText(overwrite ? QStringLiteral("Lossless stream copy\nReplaces the original recording")
                                   : QStringLiteral("Lossless stream copy\n"
                                                    "Saved beside the source as \xe2\x80\x9c<name>_edit.%1\xe2\x80\x9d")
                                         .arg(ext));
}

void ExportPanel::applyThemeStyles() {
    const auto& t = ActiveTheme();

    // Same framing as the Details card above it, so the rail reads as one column
    // of cards rather than a card plus a loose form.
    setStyleSheet(QStringLiteral("QFrame#exportPanel {"
                                 "background:%1;"
                                 "border: 1px solid %2;"
                                 "border-radius: %3px;"
                                 "}")
                      .arg(tok(t.surf), tok(t.line))
                      .arg(M::kRadiusLg));

    title_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:700; font-size:13.5px; }").arg(tok(t.ink)));

    const QString label_style = QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(tok(t.mut));
    container_label_->setStyleSheet(label_style);
    save_mode_label_->setStyleSheet(label_style);
    // Muted, not dim: the line states what pressing Export will do to the user's
    // file, which is a different weight of information from ordinary help text.
    dest_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(tok(t.mut)));

    status_separator_->setStyleSheet(QStringLiteral("QFrame { background:%1; border:none; }").arg(tok(t.line)));

    status_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12.5px; }").arg(tok(t.ink)));
    progress_bar_->setStyleSheet(QStringLiteral("QProgressBar { background:%1; border-radius:3px; border:none; }"
                                                "QProgressBar::chunk { background:%2; border-radius:3px; }")
                                     .arg(tok(t.raise), tok(t.ac)));

    result_detail_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-size:11px; }").arg(theme::ThemeText1Color(t)));

    // Badge/title colours follow the current result state; Options/Running never
    // show this content, so the icon can stay unset then.
    if (state_ == State::Done) {
        result_icon_label_->setPixmap(theme::lucidePixmap(QStringLiteral("check-circle"), tok(t.success), 16, 2.0));
        result_icon_label_->setStyleSheet(QStringLiteral("QLabel { background:%1; border:1px solid %2; "
                                                         "border-radius:14px; }")
                                              .arg(dimToken(t.success), borderToken(t.success)));
        result_title_label_->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12.5px; }").arg(tok(t.success)));
    } else if (state_ == State::Failed) {
        result_icon_label_->setPixmap(theme::lucidePixmap(QStringLiteral("x-circle"), tok(t.error), 16, 2.0));
        result_icon_label_->setStyleSheet(QStringLiteral("QLabel { background:%1; border:1px solid %2; "
                                                         "border-radius:14px; }")
                                              .arg(dimToken(t.error), borderToken(t.error)));
        result_title_label_->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; font-size:12.5px; }").arg(tok(t.error)));
    }
}

} // namespace exosnap::ui::widgets
