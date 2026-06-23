#include "RecordingErrorPanel.h"

#include "../theme/ExoSnapTheme.h"
#include "../theme/LucideIcon.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

using theme::ActiveTheme;

QString tok(const char* base) {
    return QString::fromUtf8(base);
}

// A QLabel carrying a tinted Lucide glyph at logical `size` (DPR-crisp).
QLabel* makeIconLabel(const QString& name, const char* color_base, int size, QWidget* parent) {
    auto* label = new QLabel(parent);
    const qreal dpr = parent != nullptr ? parent->devicePixelRatioF() : 1.0;
    label->setPixmap(theme::lucidePixmap(name, tok(color_base), size, dpr));
    label->setFixedSize(size, size);
    label->setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    return label;
}

// One monospace "KEY  value" row inside the detail box. `value` is selectable so
// the user can copy the error code / detail into a bug report.
QWidget* makeDetailLine(const QString& key, const QString& value, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto* key_label = new QLabel(key, row);
    key_label->setFixedWidth(66);
    key_label->setStyleSheet(QStringLiteral("font-family:'IBM Plex Mono','Consolas',monospace; font-size:10px; "
                                            "letter-spacing:0.3px; color:%1; background:transparent;")
                                 .arg(tok(ActiveTheme().dim)));
    layout->addWidget(key_label, 0, Qt::AlignTop);

    auto* value_label = new QLabel(value, row);
    value_label->setWordWrap(true);
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setStyleSheet(
        QStringLiteral(
            "font-family:'IBM Plex Mono','Consolas',monospace; font-size:11px; color:%1; background:transparent;")
            .arg(tok(ActiveTheme().ink)));
    layout->addWidget(value_label, 1);
    return row;
}

} // namespace

RecordingErrorPanel::RecordingErrorPanel(const RecordingErrorModel& model, QWidget* parent)
    : QWidget(parent), model_(model) {
    setObjectName(QStringLiteral("recordingErrorCard"));
    setFixedWidth(440);
    setStyleSheet(QStringLiteral("#recordingErrorCard { background:%1; border:1px solid %2; border-radius:14px; }")
                      .arg(tok(ActiveTheme().surf), tok(ActiveTheme().line2)));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 20, 22, 18);
    root->setSpacing(0);

    root->addWidget(buildHeader());
    root->addSpacing(14);
    root->addWidget(buildDetailBox());

    if (model_.can_send_report) {
        root->addSpacing(12);
        root->addWidget(buildPrivacyNote());
    }

    root->addSpacing(16);
    root->addWidget(buildActionsRow());
}

bool RecordingErrorPanel::canSendReport() const noexcept {
    return model_.can_send_report;
}

QWidget* RecordingErrorPanel::buildHeader() {
    auto* header = new QWidget(this);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // Warn glyph in a tinted error-tile, mirroring the crash dialog's bug tile.
    auto* tile = new QFrame(header);
    tile->setFixedSize(38, 38);
    tile->setStyleSheet(QStringLiteral("QFrame { background:%1; border:1px solid %2; border-radius:10px; }")
                            .arg(theme::ThemeRgba(theme::ParseThemeColor(ActiveTheme().error), 0.13),
                                 theme::ThemeRgba(theme::ParseThemeColor(ActiveTheme().error), 0.42)));
    auto* tile_layout = new QVBoxLayout(tile);
    tile_layout->setContentsMargins(0, 0, 0, 0);
    tile_layout->addWidget(makeIconLabel(QStringLiteral("alert-triangle"), ActiveTheme().error, 18, tile), 0,
                           Qt::AlignCenter);
    layout->addWidget(tile, 0, Qt::AlignTop);

    auto* text_col = new QWidget(header);
    auto* text_layout = new QVBoxLayout(text_col);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(4);

    auto* title = new QLabel(model_.title, text_col);
    title->setWordWrap(true);
    title->setStyleSheet(QStringLiteral("font-size:15px; font-weight:600; color:%1; background:transparent;")
                             .arg(tok(ActiveTheme().ink)));
    text_layout->addWidget(title);

    if (!model_.summary.isEmpty()) {
        auto* summary = new QLabel(model_.summary, text_col);
        summary->setWordWrap(true);
        summary->setStyleSheet(
            QStringLiteral("font-size:12.5px; color:%1; background:transparent;").arg(tok(ActiveTheme().mut)));
        text_layout->addWidget(summary);
    }

    layout->addWidget(text_col, 1);
    return header;
}

QWidget* RecordingErrorPanel::buildDetailBox() {
    auto* frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("recordingErrorDetail"));
    frame->setStyleSheet(
        QStringLiteral("#recordingErrorDetail { background:%1; border:1px solid %2; border-radius:10px; }")
            .arg(tok(ActiveTheme().bg), tok(ActiveTheme().line)));

    auto* body = new QVBoxLayout(frame);
    body->setContentsMargins(15, 13, 15, 13);
    body->setSpacing(6);

    if (!model_.phase.isEmpty())
        body->addWidget(makeDetailLine(QStringLiteral("PHASE"), model_.phase, frame));
    if (!model_.code.isEmpty())
        body->addWidget(makeDetailLine(QStringLiteral("CODE"), model_.code, frame));

    // Collapse the codec triple into one line when present (e.g. "MKV · HEVC · Opus").
    QStringList codec_bits;
    if (!model_.container.isEmpty())
        codec_bits << model_.container;
    if (!model_.video_codec.isEmpty())
        codec_bits << model_.video_codec;
    if (!model_.audio_codec.isEmpty())
        codec_bits << model_.audio_codec;
    if (!codec_bits.isEmpty())
        body->addWidget(makeDetailLine(QStringLiteral("FORMAT"), codec_bits.join(QStringLiteral(" · ")), frame));

    if (!model_.detail.isEmpty())
        body->addWidget(makeDetailLine(QStringLiteral("DETAIL"), model_.detail, frame));

    return frame;
}

QWidget* RecordingErrorPanel::buildPrivacyNote() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    layout->addWidget(makeIconLabel(QStringLiteral("lock"), ActiveTheme().dim, 12, row), 0, Qt::AlignTop);
    auto* note = new QLabel(
        QStringLiteral("Sends the error phase, code, and codec/container only — never file paths, folder names, or "
                       "recording content."),
        row);
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1; background:transparent;").arg(tok(ActiveTheme().dim)));
    layout->addWidget(note, 1);
    return row;
}

QWidget* RecordingErrorPanel::buildActionsRow() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);

    // "Send report" (primary · Studio Mint accent) — only in official builds.
    if (model_.can_send_report) {
        send_button_ = new QPushButton(QStringLiteral("Send report"), row);
        send_button_->setObjectName(QStringLiteral("recordingErrorSendButton"));
        send_button_->setCursor(Qt::PointingHandCursor);
        send_button_->setIcon(
            theme::lucideIcon(QStringLiteral("upload"), tok(ActiveTheme().ac_ink), 14, devicePixelRatioF()));
        send_button_->setIconSize(QSize(14, 14));
        send_button_->setStyleSheet(
            QStringLiteral("QPushButton { padding:9px 15px; background:%1; border:none; border-radius:9px; color:%2; "
                           "font-size:12.5px; font-weight:600; }"
                           "QPushButton:hover { background:%3; }")
                .arg(tok(ActiveTheme().ac), tok(ActiveTheme().ac_ink), theme::ThemeAccentHover(ActiveTheme())));
        connect(send_button_, &QPushButton::clicked, this, &RecordingErrorPanel::sendReportRequested);
        layout->addWidget(send_button_);
    }

    // "Open logs" (secondary · outline).
    auto* logs_button = new QPushButton(QStringLiteral("Open logs"), row);
    logs_button->setObjectName(QStringLiteral("recordingErrorLogsButton"));
    logs_button->setCursor(Qt::PointingHandCursor);
    logs_button->setStyleSheet(
        QStringLiteral("QPushButton { padding:9px 15px; background:transparent; border:1px solid %1; "
                       "border-radius:9px; color:%2; font-size:12.5px; font-weight:500; }"
                       "QPushButton:hover { border:1px solid %3; }")
            .arg(tok(ActiveTheme().line2), tok(ActiveTheme().ink),
                 ActiveTheme().line3_override ? tok(ActiveTheme().line3_override)
                                              : QStringLiteral("rgba(255, 255, 255, 0.20)")));
    connect(logs_button, &QPushButton::clicked, this, &RecordingErrorPanel::openLogsRequested);
    layout->addWidget(logs_button);

    layout->addStretch(1);

    // "Close" (ghost).
    auto* close_button = new QPushButton(QStringLiteral("Close"), row);
    close_button->setObjectName(QStringLiteral("recordingErrorCloseButton"));
    close_button->setCursor(Qt::PointingHandCursor);
    close_button->setStyleSheet(
        QStringLiteral("QPushButton { padding:9px 13px; background:transparent; border:none; color:%1; "
                       "font-size:12.5px; font-weight:500; }"
                       "QPushButton:hover { color:%2; }")
            .arg(tok(ActiveTheme().mut), tok(ActiveTheme().ink)));
    connect(close_button, &QPushButton::clicked, this, &RecordingErrorPanel::dismissRequested);
    layout->addWidget(close_button);

    return row;
}

} // namespace exosnap::ui::dialogs
