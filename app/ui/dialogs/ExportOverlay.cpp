#include "ExportOverlay.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {

using M = exosnap::ui::theme::ExoSnapMetrics;
using exosnap::ui::theme::ActiveTheme;

namespace {

// Nested-overlay dim: unlike EditExportOverlay's fully opaque backdrop (which
// occludes the window chrome behind it), this card sits inside the edit view
// that stays visible around it, so the dim only needs to read as "modal", not
// hide anything — same alpha as every other in-window card overlay.
constexpr int kBackdropAlpha = 158;

QString tok(const char* base) {
    return QString::fromUtf8(base);
}

// Derived alpha tokens for the result badge (mirrors BuildTokens() in
// ExoSnapTheme.cpp, same recipe EditExportPage's result panel uses).
QString dimToken(const char* base_css) {
    const auto& t = ActiveTheme();
    return theme::ThemeRgba(theme::ParseThemeColor(base_css), t.kind == theme::ThemeKind::Dark ? 0.13 : 0.12);
}
QString borderToken(const char* base_css) {
    const auto& t = ActiveTheme();
    return theme::ThemeRgba(theme::ParseThemeColor(base_css), t.kind == theme::ThemeKind::Dark ? 0.44 : 0.42);
}

const QString kEmptyDetail = QStringLiteral("\xe2\x80\x94");

} // namespace

ExportOverlay::ExportOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("exportOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    buildCard();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(30, 30, 30, 30);
    root->addStretch(1);
    root->addWidget(card_, 0, Qt::AlignHCenter);
    root->addStretch(1);

    refreshStateVisibility();
    applyThemeStyles();

    if (parent != nullptr)
        parent->installEventFilter(this);
}

void ExportOverlay::buildCard() {
    card_ = new QFrame(this);
    card_->setObjectName(QStringLiteral("exportOverlayCard"));
    card_->setFixedWidth(420);

    auto* card_layout = new QVBoxLayout(card_);
    card_layout->setContentsMargins(M::kSpaceXl, M::kSpaceLg, M::kSpaceXl, M::kSpaceLg);
    card_layout->setSpacing(M::kSpaceMd);
    // The overlay centres the card via heightForWidth(), which under-reports a
    // wrapped-label card's real height — force the layout's minimum to match
    // its actual content instead of letting it compress the button row.
    card_layout->setSizeConstraint(QLayout::SetMinimumSize);

    // ---- Options content: container + save-mode + static destination ----
    options_content_ = new QWidget(card_);
    auto* options_layout = new QVBoxLayout(options_content_);
    options_layout->setContentsMargins(0, 0, 0, 0);
    options_layout->setSpacing(M::kSpaceSm);

    container_label_ = new QLabel(QStringLiteral("Container:"), options_content_);
    options_layout->addWidget(container_label_);

    // Combo contents are identical to the former Output panel (same data
    // roles, read the same way by containerKey()/saveModeKey()).
    container_combo_ = new QComboBox(options_content_);
    container_combo_->setObjectName(QStringLiteral("outputContainerCombo"));
    container_combo_->addItem(QStringLiteral("MKV  \xe2\x80\x93  stream-copy, lossless"), QStringLiteral("mkv"));
    container_combo_->addItem(QStringLiteral("MP4  \xe2\x80\x93  stream-copy, lossless (ADR\xc2\xa0"
                                             "0014)"),
                              QStringLiteral("mp4"));
    options_layout->addWidget(container_combo_);

    save_mode_label_ = new QLabel(QStringLiteral("Save:"), options_content_);
    options_layout->addWidget(save_mode_label_);

    save_mode_combo_ = new QComboBox(options_content_);
    save_mode_combo_->setObjectName(QStringLiteral("outputSaveModeCombo"));
    save_mode_combo_->addItem(QStringLiteral("Save as new file  (\xe2\x80\x9c<name>_edit.<ext>\xe2\x80\x9d)"),
                              QStringLiteral("new"));
    save_mode_combo_->addItem(QStringLiteral("Overwrite original  (atomic replace)"), QStringLiteral("overwrite"));
    options_layout->addWidget(save_mode_combo_);

    // Destination is fully determined by the save mode above — no folder
    // picker (ADR 0022) — so this row is informational only.
    auto* dest_row = new QWidget(options_content_);
    auto* dest_layout = new QHBoxLayout(dest_row);
    dest_layout->setContentsMargins(0, 0, 0, 0);
    dest_layout->setSpacing(M::kSpaceSm);

    dest_title_label_ = new QLabel(QStringLiteral("Destination:"), dest_row);
    dest_folder_label_ = new QLabel(QStringLiteral("Same folder as source"), dest_row);
    dest_folder_label_->setObjectName(QStringLiteral("editExportDestFolder"));

    dest_layout->addWidget(dest_title_label_);
    dest_layout->addWidget(dest_folder_label_, 1);
    options_layout->addWidget(dest_row);

    card_layout->addWidget(options_content_);

    // ---- Running content: status line + progress bar ----
    running_content_ = new QWidget(card_);
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

    running_layout->addWidget(status_label_);
    running_layout->addWidget(progress_bar_);

    card_layout->addWidget(running_content_);

    // ---- Result content: shared shape for Done and Failed ----
    result_content_ = new QWidget(card_);
    auto* result_layout = new QVBoxLayout(result_content_);
    result_layout->setContentsMargins(0, 0, 0, 0);
    result_layout->setSpacing(M::kSpaceSm);

    result_icon_label_ = new QLabel(result_content_);
    result_icon_label_->setFixedSize(56, 56);
    result_icon_label_->setAlignment(Qt::AlignCenter);

    result_title_label_ = new QLabel(result_content_);
    result_title_label_->setObjectName(QStringLiteral("exportResultTitle"));
    result_detail_label_ = new QLabel(result_content_);
    result_detail_label_->setObjectName(QStringLiteral("exportResultDetail"));
    result_detail_label_->setWordWrap(true);

    result_layout->addWidget(result_icon_label_, 0, Qt::AlignLeft);
    result_layout->addWidget(result_title_label_);
    result_layout->addWidget(result_detail_label_);

    card_layout->addWidget(result_content_);

    // ---- Shared button row ----
    // Open folder / Reveal sit on the left (Done only); Cancel-or-Close and
    // the primary action stay right-aligned, matching the Options/Failed
    // footer shape even when the left-hand buttons are hidden.
    auto* button_row = new QHBoxLayout();
    button_row->setContentsMargins(0, 0, 0, 0);
    button_row->setSpacing(M::kSpaceSm);

    open_folder_btn_ = new QPushButton(QStringLiteral("Open folder"), card_);
    open_folder_btn_->setObjectName(QStringLiteral("exportOpenFolderBtn"));
    open_folder_btn_->setProperty("role", "ghost");

    reveal_btn_ = new QPushButton(QStringLiteral("Show in Explorer"), card_);
    reveal_btn_->setObjectName(QStringLiteral("exportRevealBtn"));
    reveal_btn_->setProperty("role", "ghost");

    cancel_btn_ = new QPushButton(card_);
    cancel_btn_->setObjectName(QStringLiteral("exportCancelBtn"));
    cancel_btn_->setProperty("role", "ghost");

    primary_btn_ = new QPushButton(card_);
    primary_btn_->setObjectName(QStringLiteral("exportPrimaryBtn"));
    primary_btn_->setProperty("role", "primary");

    button_row->addWidget(open_folder_btn_);
    button_row->addWidget(reveal_btn_);
    button_row->addStretch(1);
    button_row->addWidget(cancel_btn_);
    button_row->addWidget(primary_btn_);

    card_layout->addLayout(button_row);

    // ---- Wiring: buttons only ever emit — no self-driven state change ----
    // (the page owns every transition; see the class comment).
    connect(primary_btn_, &QPushButton::clicked, this, [this]() {
        if (state_ == State::Options)
            emit exportRequested();
        else if (state_ == State::Failed)
            emit retryRequested();
    });
    connect(cancel_btn_, &QPushButton::clicked, this, [this]() {
        if (state_ == State::Options || state_ == State::Running)
            emit cancelRequested();
        else
            emit closeRequested();
    });
    connect(open_folder_btn_, &QPushButton::clicked, this, &ExportOverlay::openFolderRequested);
    connect(reveal_btn_, &QPushButton::clicked, this, &ExportOverlay::revealFileRequested);
}

void ExportOverlay::openCard() {
    state_ = State::Options;
    progress_bar_->setValue(0);
    refreshStateVisibility();
    syncGeometryToParent();
    setVisible(true);
    raise();
}

void ExportOverlay::closeCard() {
    if (state_ == State::Running)
        return;
    setVisible(false);
}

bool ExportOverlay::isCardOpen() const noexcept {
    // isHidden(), not isVisible(): the latter also depends on every ancestor
    // actually being shown on screen, which makes it unusable in headless
    // tests where the host widget is built but never mapped. Every sibling
    // overlay (EditExportOverlay, SourcePickerOverlay, RecoveryOverlay) tracks
    // its own open/closed state the same way.
    return !isHidden();
}

ExportOverlay::State ExportOverlay::state() const noexcept {
    return state_;
}

QString ExportOverlay::containerKey() const {
    return container_combo_->currentData().toString();
}

QString ExportOverlay::saveModeKey() const {
    return save_mode_combo_->currentData().toString();
}

void ExportOverlay::showRunning() {
    state_ = State::Running;
    status_label_->setText(QStringLiteral("Exporting\xe2\x80\xa6"));
    progress_bar_->setValue(0);
    refreshStateVisibility();
}

void ExportOverlay::setProgress(int percent) {
    progress_bar_->setValue(qBound(0, percent, 100));
}

void ExportOverlay::showDone(const QString& output_path) {
    state_ = State::Done;
    QString file_name = QFileInfo(output_path).fileName();
    if (file_name.isEmpty())
        file_name = kEmptyDetail;
    result_title_label_->setText(QStringLiteral("Export complete"));
    result_detail_label_->setText(file_name);
    refreshStateVisibility();
    applyThemeStyles(); // re-derive the badge/title colours for the new state
}

void ExportOverlay::showFailed(const QString& error_message) {
    state_ = State::Failed;
    result_title_label_->setText(QStringLiteral("Export failed"));
    result_detail_label_->setText(error_message.isEmpty() ? QStringLiteral("Unknown error") : error_message);
    refreshStateVisibility();
    applyThemeStyles(); // re-derive the badge/title colours for the new state
}

void ExportOverlay::refreshStateVisibility() {
    const bool show_options = (state_ == State::Options);
    const bool show_running = (state_ == State::Running);
    const bool show_result = (state_ == State::Done || state_ == State::Failed);

    options_content_->setVisible(show_options);
    running_content_->setVisible(show_running);
    result_content_->setVisible(show_result);

    primary_btn_->setVisible(state_ == State::Options || state_ == State::Failed);
    primary_btn_->setText(state_ == State::Failed ? QStringLiteral("Retry") : QStringLiteral("Export"));

    cancel_btn_->setText(show_result ? QStringLiteral("Close") : QStringLiteral("Cancel"));

    open_folder_btn_->setVisible(state_ == State::Done);
    reveal_btn_->setVisible(state_ == State::Done);
}

void ExportOverlay::applyThemeStyles() {
    const auto& t = ActiveTheme();

    card_->setStyleSheet(QStringLiteral("QFrame#exportOverlayCard { background:%1; border:1px solid %2; "
                                        "border-radius:%3px; }")
                             .arg(tok(t.surf), tok(t.line2))
                             .arg(M::kRadiusLg));

    const QString label_style = QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(tok(t.mut));
    container_label_->setStyleSheet(label_style);
    save_mode_label_->setStyleSheet(label_style);
    dest_title_label_->setStyleSheet(label_style);
    dest_folder_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(theme::ThemeText1Color(t)));

    status_label_->setStyleSheet(
        QStringLiteral("QLabel { color:%1; font-weight:600; font-size:14px; }").arg(tok(t.ink)));
    progress_bar_->setStyleSheet(QStringLiteral("QProgressBar { background:%1; border-radius:3px; border:none; }"
                                                "QProgressBar::chunk { background:%2; border-radius:3px; }")
                                     .arg(tok(t.raise), tok(t.ac)));

    result_detail_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-size:12px; }").arg(tok(t.mut)));

    // Badge/title colours follow the current result state (success vs error);
    // Options/Running never show this content so the icon can stay unset then.
    if (state_ == State::Done) {
        result_icon_label_->setPixmap(theme::lucidePixmap(QStringLiteral("check-circle"), tok(t.success), 28, 2.0));
        result_icon_label_->setStyleSheet(QStringLiteral("QLabel { background:%1; border:1px solid %2; "
                                                         "border-radius:28px; }")
                                              .arg(dimToken(t.success), borderToken(t.success)));
        result_title_label_->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; font-size:16px; }").arg(tok(t.success)));
    } else if (state_ == State::Failed) {
        result_icon_label_->setPixmap(theme::lucidePixmap(QStringLiteral("x-circle"), tok(t.error), 28, 2.0));
        result_icon_label_->setStyleSheet(QStringLiteral("QLabel { background:%1; border:1px solid %2; "
                                                         "border-radius:28px; }")
                                              .arg(dimToken(t.error), borderToken(t.error)));
        result_title_label_->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-weight:600; font-size:16px; }").arg(tok(t.error)));
    }
}

void ExportOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

void ExportOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (state_ != State::Running)
            emit closeRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void ExportOverlay::mousePressEvent(QMouseEvent* event) {
    if (card_ == nullptr || !card_->geometry().contains(event->pos())) {
        if (state_ != State::Running)
            emit closeRequested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

bool ExportOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void ExportOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
