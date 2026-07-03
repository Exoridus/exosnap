#include "ElevationLock.h"

#include "../theme/ExoSnapMetrics.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/LucideIcon.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace exosnap::ui::widgets {

using M = theme::ExoSnapMetrics;

namespace {
QLabel* glyphLabel(QWidget* parent, const QString& name, const char* color, int size) {
    auto* l = new QLabel(parent);
    l->setFixedSize(size, size);
    l->setPixmap(theme::lucidePixmap(name, QString::fromUtf8(color), size, l->devicePixelRatioF()));
    return l;
}
} // namespace

ElevationLock::ElevationLock(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("diagElevationLock"));
    setProperty("panelRole", "elevationLock");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(M::kSpaceLg, M::kSpaceMd, M::kSpaceLg, M::kSpaceMd);
    root->setSpacing(M::kSpaceMd);

    auto* head = new QHBoxLayout();
    head->setSpacing(M::kSpaceMd);
    head->addWidget(glyphLabel(this, QStringLiteral("lock"), theme::ExoSnapPalette::kText2, 18), 0, Qt::AlignTop);

    auto* head_text = new QVBoxLayout();
    head_text->setSpacing(M::kSpaceXs);
    auto* title_row = new QHBoxLayout();
    title_row->setSpacing(M::kSpaceSm);
    auto* title = new QLabel(QStringLiteral("Elevated diagnostics"), this);
    title->setProperty("labelRole", "cardTitle");
    auto* locked = new QLabel(QStringLiteral("LOCKED"), this);
    locked->setProperty("labelRole", "elevationBadge");
    title_row->addWidget(title, 0);
    title_row->addWidget(locked, 0);
    title_row->addStretch(1);
    head_text->addLayout(title_row);
    auto* desc = new QLabel(QStringLiteral("Standard diagnostics already run on the DXGI / NVAPI baseline — including "
                                           "monitor judder, no admin needed. Elevation only adds the deeper "
                                           "present-path checks below."),
                            this);
    desc->setProperty("labelRole", "subtitle");
    desc->setWordWrap(true);
    head_text->addWidget(desc);
    head->addLayout(head_text, 1);

    // Honestly disabled (planned): the self-relaunch flow is a later slice. Mirrors
    // the Export Report pattern — never a live-looking button that silently no-ops.
    auto* restart = new QPushButton(QStringLiteral("Restart as Admin"), this);
    restart->setObjectName(QStringLiteral("elevationRestartBtn"));
    restart->setProperty("role", "ghost");
    restart->setEnabled(false);
    restart->setToolTip(QStringLiteral("Elevated relaunch is planned for a future build."));
    head->addWidget(restart, 0, Qt::AlignTop);
    root->addLayout(head);

    // Three unlock facts (what elevation actually adds).
    const struct {
        const char* glyph;
        const char* title;
        const char* detail;
    } unlocks[] = {
        {"layers", "Present-mode & tearing", "PresentMon — window / game capture"},
        {"activity", "DPC / ISR latency + culprit driver", "who is stalling the present path"},
        {"lock", "Capture elevated games", "UIPI-protected fullscreen titles"},
    };

    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(M::kSpaceLg);
    grid->setVerticalSpacing(M::kSpaceXs);
    int col = 0;
    for (const auto& u : unlocks) {
        auto* cell = new QVBoxLayout();
        cell->setSpacing(M::kSpaceXs);
        cell->addWidget(glyphLabel(this, QString::fromUtf8(u.glyph), theme::ExoSnapPalette::kText3, 15), 0,
                        Qt::AlignLeft);
        auto* ut = new QLabel(QString::fromUtf8(u.title), this);
        ut->setProperty("labelRole", "body");
        ut->setWordWrap(true);
        auto* ud = new QLabel(QString::fromUtf8(u.detail), this);
        ud->setProperty("labelRole", "subtle");
        ud->setWordWrap(true);
        cell->addWidget(ut);
        cell->addWidget(ud);
        auto* cell_host = new QWidget(this);
        cell_host->setLayout(cell);
        grid->addWidget(cell_host, 0, col++);
    }
    root->addLayout(grid);

    auto* footer = new QHBoxLayout();
    footer->setSpacing(M::kSpaceSm);
    footer->addWidget(glyphLabel(this, QStringLiteral("info"), theme::ExoSnapPalette::kText3, 13), 0, Qt::AlignVCenter);
    auto* foot = new QLabel(QStringLiteral("Opt-in · relaunch required · disabled during recording."), this);
    foot->setProperty("labelRole", "subtle");
    footer->addWidget(foot, 1);
    root->addLayout(footer);
}

} // namespace exosnap::ui::widgets
