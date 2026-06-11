#include "KeycapChip.h"

#include <QHBoxLayout>
#include <QKeySequence>
#include <QLayoutItem>
#include <QStyle>

namespace exosnap::ui::widgets {

KeycapChip::KeycapChip(const QString& key_text, QWidget* parent) : QLabel(key_text, parent) {
    setObjectName(QStringLiteral("keycap"));
    setProperty("labelRole", "keycap");
    setAlignment(Qt::AlignCenter);
    setTextInteractionFlags(Qt::NoTextInteraction);
}

void KeycapChip::setMuted(bool muted) {
    if (muted_ == muted)
        return;
    muted_ = muted;
    setProperty("stateRole", muted ? QStringLiteral("muted") : QVariant());
    style()->unpolish(this);
    style()->polish(this);
}

int populateKeycaps(QHBoxLayout* layout, const QKeySequence& seq, QWidget* parent, const QString& empty_text) {
    if (!layout)
        return 0;

    // Delete immediately (not deleteLater) so re-population leaves no stale chip children behind
    // even without a running event loop. Safe: never invoked from a chip's own signal handler.
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QString text = seq.isEmpty() ? QString() : seq.toString(QKeySequence::NativeText);
    if (text.isEmpty()) {
        auto* chip = new KeycapChip(empty_text.isEmpty() ? QStringLiteral("Unset") : empty_text, parent);
        chip->setMuted(true);
        layout->addWidget(chip);
        return 0;
    }

    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    int count = 0;
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            auto* plus = new QLabel(QStringLiteral("+"), parent);
            plus->setProperty("labelRole", "keycapPlus");
            layout->addWidget(plus);
        }
        layout->addWidget(new KeycapChip(parts.at(i).trimmed(), parent));
        ++count;
    }
    return count;
}

} // namespace exosnap::ui::widgets
