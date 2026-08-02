#include "EditDetailsRail.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapTheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

using M = exosnap::ui::theme::ExoSnapMetrics;
using exosnap::ui::theme::ActiveTheme;

namespace {
// Unified empty-value glyph, shared with EditExportPage's own fact rows.
const QString kEmptyValue = QStringLiteral("\xe2\x80\x94");
} // namespace

EditDetailsRail::EditDetailsRail(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("editDetailsRail"));
    // The rail is a fixed-width column wherever it is embedded — the page that
    // hosts it does not need to wrap it in its own fixed-width container.
    setFixedWidth(280);

    rail_layout_ = new QVBoxLayout(this);
    rail_layout_->setContentsMargins(M::kSpaceMd, M::kSpaceMd, M::kSpaceMd, M::kSpaceMd);
    rail_layout_->setSpacing(0);

    title_label_ = new QLabel(QStringLiteral("Details"), this);
    title_label_->setObjectName(QStringLiteral("editDetailsRailTitle"));
    rail_layout_->addWidget(title_label_);
    rail_layout_->addSpacing(M::kSpaceSm);

    addFactRow(QStringLiteral("Duration"), duration_value_, /*first=*/true);
    duration_value_->setObjectName(QStringLiteral("factDurationValue"));

    addFactRow(QStringLiteral("Size"), size_value_, /*first=*/false);
    size_value_->setObjectName(QStringLiteral("factSizeValue"));

    addFactRow(QStringLiteral("Resolution"), resolution_value_, /*first=*/false);
    resolution_value_->setObjectName(QStringLiteral("factResolutionValue"));

    addFactRow(QStringLiteral("Frame rate"), fps_value_, /*first=*/false);
    fps_value_->setObjectName(QStringLiteral("factFpsValue"));

    addFactRow(QStringLiteral("Video"), video_value_, /*first=*/false);
    video_value_->setObjectName(QStringLiteral("factVideoValue"));

    addFactRow(QStringLiteral("Audio"), audio_value_, /*first=*/false);
    audio_value_->setObjectName(QStringLiteral("factAudioValue"));

    addFactRow(QStringLiteral("Container"), container_value_, /*first=*/false);
    container_value_->setObjectName(QStringLiteral("factContainerValue"));

    applyThemeStyles();
}

void EditDetailsRail::addFactRow(const QString& key_text, QLabel*& value_out, bool first) {
    if (!first) {
        auto* sep = new QFrame(this);
        sep->setFixedHeight(1);
        separators_.push_back(sep);
        rail_layout_->addWidget(sep);
    }

    auto* row = new QWidget(this);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 7, 0, 7);
    row_layout->setSpacing(M::kSpaceSm);

    auto* key = new QLabel(key_text, row);
    value_out = new QLabel(kEmptyValue, row);
    value_out->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    row_layout->addWidget(key);
    row_layout->addWidget(value_out, 1);
    rail_layout_->addWidget(row);

    rows_.push_back(FactRow{key, value_out});
}

void EditDetailsRail::setFacts(const Facts& facts) {
    duration_value_->setText(facts.duration.isEmpty() ? kEmptyValue : facts.duration);
    size_value_->setText(facts.size.isEmpty() ? kEmptyValue : facts.size);
    resolution_value_->setText(facts.resolution.isEmpty() ? kEmptyValue : facts.resolution);
    fps_value_->setText(facts.fps.isEmpty() ? kEmptyValue : facts.fps);
    video_value_->setText(facts.video_codec.isEmpty() ? kEmptyValue : facts.video_codec);
    audio_value_->setText(facts.audio_codec.isEmpty() ? kEmptyValue : facts.audio_codec);
    container_value_->setText(facts.container.isEmpty() ? kEmptyValue : facts.container);
}

void EditDetailsRail::applyThemeStyles() {
    const auto& t = ActiveTheme();

    setStyleSheet(QStringLiteral("QFrame#editDetailsRail {"
                                 "background:%1;"
                                 "border: 1px solid %2;"
                                 "border-radius: %3px;"
                                 "}")
                      .arg(t.surf, t.line)
                      .arg(M::kRadiusLg));

    title_label_->setStyleSheet(QStringLiteral("QLabel { color:%1; font-weight:700; font-size:13.5px; }").arg(t.ink));

    for (QFrame* sep : separators_)
        sep->setStyleSheet(QStringLiteral("QFrame { background:%1; border:none; }").arg(t.line));

    for (const FactRow& row : rows_) {
        row.key->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:11px; }")
                .arg(t.dim));
        row.value->setStyleSheet(
            QStringLiteral("QLabel { color:%1; font-family:'IBM Plex Mono','Consolas',monospace; font-size:12px; }")
                .arg(t.ink));
    }
}

} // namespace exosnap::ui::widgets
