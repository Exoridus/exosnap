#include "PlaceholderRow.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>

#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"

namespace exosnap::ui::widgets {

using namespace exosnap::ui::theme;

PlaceholderRow::PlaceholderRow(QWidget* parent) : QWidget(parent) {
    setProperty("widgetRole", "placeholderRow");

    label_ = new QLabel(this);
    {
        QFont f = label_->font();
        f.setPixelSize(13);
        // Slightly larger than default; use setPointSizeF for cross-DPI safety.
        // 13.5px ≈ 10pt at 96 DPI — set via pixel size to match the spec.
        f.setPixelSize(14); // closest to 13.5px in integer pixel sizes
        label_->setFont(f);
        label_->setStyleSheet(
            QStringLiteral("color: %1;").arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim)));
    }

    version_badge_ = new QLabel(this);
    {
        QFont f;
        f.setFamily(QStringLiteral("IBM Plex Mono"));
        f.setPixelSize(10); // closest integer to 9.5px
        version_badge_->setFont(f);
        version_badge_->setStyleSheet(
            QStringLiteral("color: %1; padding: 2px 6px; border: 1px dashed %2; border-radius: 5px;")
                .arg(QString::fromUtf8(exosnap::ui::theme::ActiveTheme().dim),
                     QString::fromUtf8(exosnap::ui::theme::ActiveTheme().line2)));
    }

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(label_);
    layout->addStretch();
    layout->addWidget(version_badge_);
}

void PlaceholderRow::setLabel(const QString& label) {
    label_->setText(label);
}

void PlaceholderRow::setVersionTag(const QString& version) {
    version_badge_->setText(version);
}

} // namespace exosnap::ui::widgets
