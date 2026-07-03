#include "TipChip.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

using M = theme::ExoSnapMetrics;

TipChip::TipChip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("diagTipChip"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(M::kSpaceSm);

    head_ = new QToolButton(this);
    head_->setObjectName(QStringLiteral("tipChipHead"));
    head_->setProperty("role", "tipChipHead");
    head_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    head_->setCheckable(true);
    head_->setCursor(Qt::PointingHandCursor);
    head_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    root->addWidget(head_, 0, Qt::AlignLeft);

    body_ = new QWidget(this);
    body_->setProperty("panelRole", "panel");
    body_layout_ = new QVBoxLayout(body_);
    body_layout_->setContentsMargins(0, 0, 0, 0);
    body_layout_->setSpacing(0);
    body_->setVisible(false);
    root->addWidget(body_);

    connect(head_, &QToolButton::toggled, this, [this](bool on) { body_->setVisible(on && !tips_.isEmpty()); });

    setVisible(false);
}

void TipChip::setDefaultOpen(bool open) {
    default_open_ = open;
    head_->setChecked(open);
    body_->setVisible(open && !tips_.isEmpty());
}

int TipChip::tipCount() const noexcept {
    return static_cast<int>(tips_.size());
}

void TipChip::setTips(const QVector<Tip>& tips) {
    tips_ = tips;
    rebuild();
}

void TipChip::rebuild() {
    // Clear the body.
    QLayoutItem* child = nullptr;
    while ((child = body_layout_->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }

    if (tips_.isEmpty()) {
        setVisible(false);
        return;
    }
    setVisible(true);

    const int n = tipCount();
    head_->setText(n == 1 ? QStringLiteral("1 tip to optimise") : QStringLiteral("%1 tips to optimise").arg(n));
    const qreal dpr = head_->devicePixelRatioF();
    head_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    head_->setIcon(
        QIcon(theme::lucidePixmap(QStringLiteral("zap"), QString::fromUtf8(theme::ExoSnapPalette::kAccent), 14, dpr)));

    bool first = true;
    for (const Tip& t : tips_) {
        auto* row = new QWidget(body_);
        row->setProperty("tipRow", true);
        row->setProperty("firstRow", first);
        auto* rl = new QHBoxLayout(row);
        rl->setContentsMargins(M::kSpaceMd, M::kSpaceSm, M::kSpaceMd, M::kSpaceSm);
        rl->setSpacing(M::kSpaceMd);

        auto* glyph = new QLabel(row);
        glyph->setFixedSize(14, 14);
        glyph->setPixmap(theme::lucidePixmap(t.glyph.isEmpty() ? QStringLiteral("zap") : t.glyph,
                                             QString::fromUtf8(theme::ExoSnapPalette::kAccent), 14,
                                             glyph->devicePixelRatioF()));
        rl->addWidget(glyph, 0, Qt::AlignTop);

        auto* text_col = new QVBoxLayout();
        text_col->setSpacing(1);
        auto* summary = new QLabel(t.summary, row);
        summary->setProperty("labelRole", "tipSummary");
        summary->setWordWrap(true);
        auto* id = new QLabel(t.id, row);
        id->setProperty("labelRole", "tipId");
        text_col->addWidget(summary);
        text_col->addWidget(id);
        rl->addLayout(text_col, 1);

        if (!t.fix_label.isEmpty()) {
            const QString fix_id = t.fix_id;
            const QString changes = t.changes;
            if (t.fix_kind == 2) {
                // External — no button, informational label only.
                auto* lbl = new QLabel(t.fix_label, row);
                lbl->setProperty("labelRole", "tipId");
                rl->addWidget(lbl, 0, Qt::AlignVCenter);
            } else if (t.fix_kind == 1) {
                auto* btn = new QPushButton(t.fix_label + QStringLiteral(" \xE2\x86\x92"), row);
                btn->setProperty("role", "ghost");
                btn->setObjectName(QStringLiteral("tipFixBtn"));
                connect(btn, &QPushButton::clicked, this, [this, fix_id]() { emit assistedFixRequested(fix_id); });
                rl->addWidget(btn, 0, Qt::AlignVCenter);
            } else {
                auto* btn = new QPushButton(t.fix_label, row);
                btn->setProperty("role", "ghost");
                btn->setObjectName(QStringLiteral("tipFixBtn"));
                connect(btn, &QPushButton::clicked, this,
                        [this, fix_id, changes]() { emit applyFixRequested(fix_id, changes); });
                rl->addWidget(btn, 0, Qt::AlignVCenter);
            }
        }

        body_layout_->addWidget(row);
        first = false;
    }

    body_->setVisible(head_->isChecked());
}

} // namespace exosnap::ui::widgets
