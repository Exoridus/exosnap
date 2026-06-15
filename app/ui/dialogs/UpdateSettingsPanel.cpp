#include "UpdateSettingsPanel.h"

#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

#include <QButtonGroup>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSize>
#include <QString>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

using theme::ExoSnapPalette;

QString tok(const char* base) {
    return QString::fromLatin1(base);
}

// Derive the dim/border variants of a semantic colour at the design's frozen alphas
// (dim 0.13 · border 0.42) so there are no colour literals at call sites — matches
// CrashReportPanel's convention.
QString rgba(const char* base, double alpha) {
    QColor c(QString::fromLatin1(base));
    c.setAlphaF(alpha);
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

constexpr const char* kMono = "'IBM Plex Mono','Consolas',monospace";

// A QLabel carrying a tinted Lucide glyph at logical `size` (DPR-crisp).
QLabel* makeIconLabel(const QString& name, const char* color_base, int size, QWidget* parent) {
    auto* label = new QLabel(parent);
    const qreal dpr = parent != nullptr ? parent->devicePixelRatioF() : 1.0;
    label->setPixmap(theme::lucidePixmap(name, tok(color_base), size, dpr));
    label->setFixedSize(size, size);
    label->setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    return label;
}

// One bulleted "What's new" line.
QWidget* makeBulletRow(const QString& text, QWidget* parent) {
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(9);

    auto* dot = new QFrame(row);
    dot->setFixedSize(5, 5);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:2px;").arg(tok(ExoSnapPalette::kAccent)));
    layout->addWidget(dot, 0, Qt::AlignTop);

    auto* label = new QLabel(text, row);
    label->setWordWrap(true);
    label->setStyleSheet(
        QStringLiteral("font-size:12.5px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText2)));
    layout->addWidget(label, 1);
    return row;
}

// A small caution/info banner (icon + text), tone-tinted.
QFrame* makeBanner(const QString& object_name, const QString& icon_name, const char* tone_base, const QString& text,
                   QWidget* parent) {
    auto* banner = new QFrame(parent);
    banner->setObjectName(object_name);
    banner->setStyleSheet(QStringLiteral("#%1 { background:%2; border:1px solid %3; border-radius:10px; }")
                              .arg(object_name, rgba(tone_base, 0.13), rgba(tone_base, 0.42)));
    auto* layout = new QHBoxLayout(banner);
    layout->setContentsMargins(13, 10, 13, 10);
    layout->setSpacing(10);
    layout->addWidget(makeIconLabel(icon_name, tone_base, 16, banner), 0, Qt::AlignTop);
    auto* label = new QLabel(text, banner);
    label->setWordWrap(true);
    label->setStyleSheet(
        QStringLiteral("font-size:12.5px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText0)));
    layout->addWidget(label, 1);
    return banner;
}

QFrame* makeDivider(QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setFixedHeight(1);
    divider->setStyleSheet(QStringLiteral("background:%1; border:none;").arg(tok(ExoSnapPalette::kLine1)));
    return divider;
}

} // namespace

UpdateSettingsPanel::UpdateSettingsPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("updateSettingsPanel"));
    setStyleSheet(QStringLiteral("#updateSettingsPanel { background:%1; border:1px solid %2; border-radius:14px; }")
                      .arg(tok(ExoSnapPalette::kBg2), tok(ExoSnapPalette::kLine1)));

    // Persistent "Check for updates" button (re-parented into the per-state body).
    check_button_ = new QPushButton(QStringLiteral("Check for updates"), this);
    check_button_->setObjectName(QStringLiteral("updateCheckButton"));
    check_button_->setCursor(Qt::PointingHandCursor);
    check_button_->setStyleSheet(
        QStringLiteral(
            "QPushButton { background:%1; color:%2; border:1px solid %3; border-radius:8px; padding:8px 14px; "
            "font-size:12.5px; }"
            "QPushButton:hover { background:%4; }"
            "QPushButton:disabled { color:%5; }")
            .arg(tok(ExoSnapPalette::kBg3), tok(ExoSnapPalette::kText0), tok(ExoSnapPalette::kLine2),
                 tok(ExoSnapPalette::kBg4), tok(ExoSnapPalette::kText3)));
    connect(check_button_, &QPushButton::clicked, this, &UpdateSettingsPanel::checkRequested);

    // Persistent Stable/Preview segmented control (built from two exclusive toggles —
    // no reusable Segmented widget exists in the repo).
    channel_group_ = new QButtonGroup(this);
    channel_group_->setExclusive(true);

    const QString seg_style =
        QStringLiteral(
            "QPushButton { background:transparent; color:%1; border:none; border-radius:6px; padding:5px 14px; "
            "font-size:12px; }"
            "QPushButton:checked { background:%2; color:%3; }"
            "QPushButton:hover:!checked { color:%4; }")
            .arg(tok(ExoSnapPalette::kText2), tok(ExoSnapPalette::kBg4), tok(ExoSnapPalette::kText0),
                 tok(ExoSnapPalette::kText0));

    channel_stable_ = new QPushButton(QStringLiteral("Stable"), this);
    channel_stable_->setObjectName(QStringLiteral("updateChannelStable"));
    channel_stable_->setCheckable(true);
    channel_stable_->setCursor(Qt::PointingHandCursor);
    channel_stable_->setStyleSheet(seg_style);
    channel_group_->addButton(channel_stable_, 0);

    channel_preview_ = new QPushButton(QStringLiteral("Preview"), this);
    channel_preview_->setObjectName(QStringLiteral("updateChannelPreview"));
    channel_preview_->setCheckable(true);
    channel_preview_->setCursor(Qt::PointingHandCursor);
    channel_preview_->setStyleSheet(seg_style);
    channel_group_->addButton(channel_preview_, 1);

    channel_stable_->setChecked(true);
    model_.channel = QStringLiteral("Stable");

    // channel applies immediately in 0.4.0 (persist + re-check) — no restart dance.
    connect(channel_group_, &QButtonGroup::idClicked, this, [this](int id) {
        const QString next = id == 1 ? QStringLiteral("Preview") : QStringLiteral("Stable");
        if (next == model_.channel) {
            rebuild();
            return;
        }
        model_.channel = next;
        rebuild();
        emit channelChanged(next);
    });

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(0);

    root->addWidget(buildHeader());

    auto* body = new QWidget(this);
    body_layout_ = new QVBoxLayout(body);
    body_layout_->setContentsMargins(0, 16, 0, 0);
    body_layout_->setSpacing(0);
    root->addWidget(body);

    rebuild();
}

QString UpdateSettingsPanel::channel() const {
    return model_.channel;
}

void UpdateSettingsPanel::setState(UpdateUiState state) {
    state_ = state;
    rebuild();
}

void UpdateSettingsPanel::setModel(const UpdateUiModel& model) {
    model_ = model;
    if (!model_.channel.isEmpty())
        selectChannelButton(model_.channel);
    rebuild();
}

void UpdateSettingsPanel::setRecordingActive(bool active) {
    model_.recording_active = active;
    rebuild();
}

void UpdateSettingsPanel::selectChannelButton(const QString& channel) {
    const bool preview = channel.compare(QStringLiteral("Preview"), Qt::CaseInsensitive) == 0;
    QSignalBlocker block_stable(channel_stable_);
    QSignalBlocker block_preview(channel_preview_);
    channel_preview_->setChecked(preview);
    channel_stable_->setChecked(!preview);
}

QWidget* UpdateSettingsPanel::buildHeader() {
    auto* row = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    header_tile_ = new QLabel(row);
    header_tile_->setFixedSize(34, 34);
    header_tile_->setAlignment(Qt::AlignCenter);
    layout->addWidget(header_tile_, 0, Qt::AlignTop);

    auto* text_col = new QWidget(row);
    auto* text_layout = new QVBoxLayout(text_col);
    text_layout->setContentsMargins(0, 0, 0, 0);
    text_layout->setSpacing(1);

    header_title_ = new QLabel(text_col);
    header_title_->setObjectName(QStringLiteral("updateHeaderTitle"));
    header_title_->setStyleSheet(QStringLiteral("font-size:14.5px; font-weight:600; color:%1; background:transparent;")
                                     .arg(tok(ExoSnapPalette::kText0)));
    text_layout->addWidget(header_title_);

    header_sub_ = new QLabel(text_col);
    header_sub_->setObjectName(QStringLiteral("updateHeaderSub"));
    header_sub_->setWordWrap(true);
    header_sub_->setStyleSheet(QStringLiteral("font-family:%1; font-size:11.5px; color:%2; background:transparent;")
                                   .arg(QString::fromLatin1(kMono), tok(ExoSnapPalette::kText2)));
    text_layout->addWidget(header_sub_);

    layout->addWidget(text_col, 1);

    version_pill_ = new QLabel(row);
    version_pill_->setObjectName(QStringLiteral("updateVersionPill"));
    version_pill_->setStyleSheet(
        QStringLiteral(
            "font-family:%1; font-size:11px; color:%2; background:%3; border:1px solid %4; border-radius:6px; "
            "padding:3px 9px;")
            .arg(QString::fromLatin1(kMono), tok(ExoSnapPalette::kText3), tok(ExoSnapPalette::kBg3),
                 tok(ExoSnapPalette::kLine1)));
    version_pill_->setVisible(false);
    layout->addWidget(version_pill_, 0, Qt::AlignVCenter);

    return row;
}

QWidget* UpdateSettingsPanel::buildChannelSelector() {
    auto* wrap = new QWidget(this);
    auto* outer = new QVBoxLayout(wrap);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addWidget(makeDivider(wrap));
    outer->addSpacing(14);

    auto* row = new QWidget(wrap);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(12);

    auto* label = new QLabel(QStringLiteral("Update channel"), row);
    label->setStyleSheet(
        QStringLiteral("font-size:13px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText0)));
    row_layout->addWidget(label, 1);

    // Segmented track: a rounded surface holding the two exclusive toggles.
    auto* segmented = new QFrame(row);
    segmented->setObjectName(QStringLiteral("updateChannelSegmented"));
    segmented->setStyleSheet(
        QStringLiteral("#updateChannelSegmented { background:%1; border:1px solid %2; border-radius:8px; }")
            .arg(tok(ExoSnapPalette::kBg3), tok(ExoSnapPalette::kLine1)));
    auto* seg_layout = new QHBoxLayout(segmented);
    seg_layout->setContentsMargins(3, 3, 3, 3);
    seg_layout->setSpacing(3);
    seg_layout->addWidget(channel_stable_);
    seg_layout->addWidget(channel_preview_);
    row_layout->addWidget(segmented, 0, Qt::AlignVCenter);

    outer->addWidget(row);
    outer->addSpacing(8);

    const bool preview = model_.channel.compare(QStringLiteral("Preview"), Qt::CaseInsensitive) == 0;
    auto* hint = new QLabel(preview ? QStringLiteral("Early builds — expect rough edges.")
                                    : QStringLiteral("Conservative, fully-tested releases."),
                            wrap);
    hint->setObjectName(QStringLiteral("updateChannelHint"));
    hint->setWordWrap(true);
    hint->setStyleSheet(
        QStringLiteral("font-size:11.5px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText3)));
    outer->addWidget(hint);

    return wrap;
}

QWidget* UpdateSettingsPanel::buildRecordingBanner() {
    return makeBanner(QStringLiteral("updateRecordingBanner"), QStringLiteral("alert-triangle"), ExoSnapPalette::kWarn,
                      QStringLiteral("Update checks are paused while recording."), this);
}

void UpdateSettingsPanel::clearBody() {
    if (body_layout_ == nullptr)
        return;
    // The persistent controls must survive a rebuild — detach them first.
    for (QPushButton* keep : {check_button_, channel_stable_, channel_preview_}) {
        if (keep != nullptr) {
            keep->setParent(this);
            keep->hide();
        }
    }
    QLayoutItem* item = nullptr;
    while ((item = body_layout_->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget())
            w->deleteLater();
        delete item;
    }
}

void UpdateSettingsPanel::rebuild() {
    clearBody();
    if (body_layout_ == nullptr)
        return;

    // ── Header (icon tile + title + subline + cur→next pill) ──────────────────
    struct HeaderSpec {
        QString icon;
        const char* tone;
    };
    HeaderSpec head;
    switch (state_) {
    case UpdateUiState::Available:
        head = {QStringLiteral("download"), ExoSnapPalette::kInfo};
        header_title_->setText(QStringLiteral("Update available"));
        header_sub_->setText(
            QStringLiteral("ExoSnap %1 · %2 channel")
                .arg(model_.available_version.isEmpty() ? QStringLiteral("—") : model_.available_version,
                     model_.channel));
        break;
    case UpdateUiState::Checking:
        head = {QStringLiteral("refresh-cw"), ExoSnapPalette::kInfo};
        header_title_->setText(QStringLiteral("Checking for updates…"));
        header_sub_->setText(QStringLiteral("%1 channel").arg(model_.channel));
        break;
    case UpdateUiState::Error:
        head = {QStringLiteral("alert-triangle"), ExoSnapPalette::kWarn};
        header_title_->setText(QStringLiteral("Couldn't check for updates"));
        header_sub_->setText(QStringLiteral("%1 channel").arg(model_.channel));
        break;
    case UpdateUiState::UpToDate:
    default:
        head = {QStringLiteral("check"), ExoSnapPalette::kOk};
        header_title_->setText(QStringLiteral("You're up to date"));
        header_sub_->setText(QStringLiteral("ExoSnap %1")
                                 .arg(model_.current_version.isEmpty() ? QStringLiteral("—") : model_.current_version));
        break;
    }
    header_tile_->setPixmap(theme::lucidePixmap(head.icon, tok(head.tone), 17, devicePixelRatioF()));
    header_tile_->setStyleSheet(QStringLiteral("background:%1; border:1px solid %2; border-radius:10px;")
                                    .arg(rgba(head.tone, 0.13), rgba(head.tone, 0.42)));

    // cur → next pill — Available only.
    if (state_ == UpdateUiState::Available && !model_.current_version.isEmpty() &&
        !model_.available_version.isEmpty()) {
        version_pill_->setText(
            QStringLiteral("%1 \xe2\x86\x92 %2").arg(model_.current_version, model_.available_version));
        version_pill_->setVisible(true);
    } else {
        version_pill_->setVisible(false);
    }

    // ── Recording caution (any state): checks are paused while capturing ──────
    if (model_.recording_active) {
        body_layout_->addWidget(buildRecordingBanner());
        body_layout_->addSpacing(14);
    }
    check_button_->setEnabled(!model_.recording_active);

    // ── Per-state body ───────────────────────────────────────────────────────
    switch (state_) {
    case UpdateUiState::Available: {
        // What's new (bulleted highlights + release-notes external link).
        auto* whats_new = new QWidget(this);
        auto* wn_layout = new QVBoxLayout(whats_new);
        wn_layout->setContentsMargins(0, 0, 0, 0);
        wn_layout->setSpacing(9);

        auto* heading_row = new QWidget(whats_new);
        auto* heading_layout = new QHBoxLayout(heading_row);
        heading_layout->setContentsMargins(0, 0, 0, 0);
        heading_layout->setSpacing(8);
        auto* heading = new QLabel(QStringLiteral("WHAT'S NEW"), heading_row);
        heading->setStyleSheet(QStringLiteral("font-family:%1; font-size:10px; letter-spacing:0.6px; color:%2; "
                                              "background:transparent;")
                                   .arg(QString::fromLatin1(kMono), tok(ExoSnapPalette::kText3)));
        heading_layout->addWidget(heading, 1);

        auto* notes_link = new QPushButton(QStringLiteral("Release notes"), heading_row);
        notes_link->setObjectName(QStringLiteral("updateReleaseNotesLink"));
        notes_link->setCursor(Qt::PointingHandCursor);
        notes_link->setIcon(
            theme::lucideIcon(QStringLiteral("external-link"), tok(ExoSnapPalette::kAccent), 12, devicePixelRatioF()));
        notes_link->setIconSize(QSize(12, 12));
        notes_link->setLayoutDirection(Qt::RightToLeft); // icon trails the label
        notes_link->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; border:none; color:%1; font-size:12px; }"
                           "QPushButton:hover { color:%2; }")
                .arg(tok(ExoSnapPalette::kAccent), tok(ExoSnapPalette::kAccentHover)));
        connect(notes_link, &QPushButton::clicked, this, &UpdateSettingsPanel::openReleaseNotesRequested);
        heading_layout->addWidget(notes_link, 0, Qt::AlignVCenter);
        wn_layout->addWidget(heading_row);

        auto* list = new QWidget(whats_new);
        list->setObjectName(QStringLiteral("updateWhatsNewList"));
        auto* list_layout = new QVBoxLayout(list);
        list_layout->setContentsMargins(0, 0, 0, 0);
        list_layout->setSpacing(6);
        for (const QString& item : model_.whats_new)
            list_layout->addWidget(makeBulletRow(item, list));
        wn_layout->addWidget(list);

        body_layout_->addWidget(whats_new);
        body_layout_->addSpacing(14);

        body_layout_->addWidget(buildChannelSelector());
        body_layout_->addSpacing(16);

        // Actions: 0.4.0 replaces the design's in-place "Download update" with a
        // manual "Open releases page" hand-off.
        // 0.4.0: notify + manual download; in-place updater deferred (Update C)
        auto* actions = new QWidget(this);
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        actions_layout->setSpacing(9);

        auto* open_btn = new QPushButton(QStringLiteral("Open releases page"), actions);
        open_btn->setObjectName(QStringLiteral("updateOpenReleasesButton"));
        open_btn->setCursor(Qt::PointingHandCursor);
        open_btn->setIcon(theme::lucideIcon(QStringLiteral("external-link"), tok(ExoSnapPalette::kAccentInk), 14,
                                            devicePixelRatioF()));
        open_btn->setIconSize(QSize(14, 14));
        open_btn->setStyleSheet(
            QStringLiteral("QPushButton { background:%1; color:%2; border:none; border-radius:8px; padding:8px 14px; "
                           "font-size:12.5px; font-weight:600; }"
                           "QPushButton:hover { background:%3; }"
                           "QPushButton:pressed { background:%4; }")
                .arg(tok(ExoSnapPalette::kAccent), tok(ExoSnapPalette::kAccentInk), tok(ExoSnapPalette::kAccentHover),
                     tok(ExoSnapPalette::kAccentPressed)));
        connect(open_btn, &QPushButton::clicked, this, &UpdateSettingsPanel::openReleasesPageRequested);
        actions_layout->addWidget(open_btn);

        actions_layout->addStretch(1);

        auto* later_btn = new QPushButton(QStringLiteral("Remind me later"), actions);
        later_btn->setObjectName(QStringLiteral("updateRemindLaterButton"));
        later_btn->setCursor(Qt::PointingHandCursor);
        later_btn->setStyleSheet(
            QStringLiteral("QPushButton { background:transparent; color:%1; border:none; font-size:12.5px; }"
                           "QPushButton:hover { color:%2; }")
                .arg(tok(ExoSnapPalette::kText2), tok(ExoSnapPalette::kText0)));
        connect(later_btn, &QPushButton::clicked, this, &UpdateSettingsPanel::remindLaterRequested);
        actions_layout->addWidget(later_btn);

        body_layout_->addWidget(actions);
        break;
    }

    case UpdateUiState::Checking: {
        auto* note = new QLabel(QStringLiteral("Contacting the update server…"), this);
        note->setWordWrap(true);
        note->setStyleSheet(
            QStringLiteral("font-size:12.5px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText2)));
        body_layout_->addWidget(note);
        body_layout_->addWidget(buildChannelSelector());
        break;
    }

    case UpdateUiState::Error: {
        body_layout_->addWidget(makeBanner(
            QStringLiteral("updateErrorBanner"), QStringLiteral("alert-triangle"), ExoSnapPalette::kWarn,
            model_.error_message.isEmpty() ? QStringLiteral("Couldn't reach the update server.") : model_.error_message,
            this));
        body_layout_->addSpacing(16);

        auto* actions = new QWidget(this);
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        actions_layout->setSpacing(9);
        check_button_->setText(QStringLiteral("Try again"));
        check_button_->show();
        actions_layout->addWidget(check_button_);
        actions_layout->addStretch(1);
        body_layout_->addWidget(actions);
        break;
    }

    case UpdateUiState::UpToDate:
    default: {
        if (!model_.last_checked.isEmpty()) {
            auto* checked = new QLabel(QStringLiteral("Last checked %1").arg(model_.last_checked), this);
            checked->setStyleSheet(
                QStringLiteral("font-size:12px; color:%1; background:transparent;").arg(tok(ExoSnapPalette::kText3)));
            body_layout_->addWidget(checked);
            body_layout_->addSpacing(14);
        }

        body_layout_->addWidget(buildChannelSelector());
        body_layout_->addSpacing(16);

        auto* actions = new QWidget(this);
        auto* actions_layout = new QHBoxLayout(actions);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        actions_layout->setSpacing(9);
        check_button_->setText(QStringLiteral("Check for updates"));
        check_button_->show();
        actions_layout->addWidget(check_button_);
        actions_layout->addStretch(1);
        body_layout_->addWidget(actions);
        break;
    }
    }
}

} // namespace exosnap::ui::dialogs
