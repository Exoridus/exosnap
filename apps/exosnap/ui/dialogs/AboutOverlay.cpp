#include "AboutOverlay.h"
#include "ExoSnapBuildInfo.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"

#ifndef EXOSNAP_BUILD_CONFIG
#define EXOSNAP_BUILD_CONFIG "Unknown"
#endif

#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShowEvent>
#include <QUrl>
#include <QVBoxLayout>

namespace exosnap::ui::dialogs {
namespace {

constexpr const char* kAppAuthor = "Exoridus";
// Canonical repository URL — taken from the configured git remote (git@github.com:Exoridus/exosnap),
// not invented. The GitHub action is only shown because this URL is real.
constexpr const char* kGitHubUrl = "https://github.com/Exoridus/exosnap";
constexpr const char* kAppDescription =
    "A calm, preview-first screen recorder with a high-performance GPU pipeline, multi-track audio "
    "routing, and diagnostics when you need them.";

// Backdrop tint behind the card — matches the Hybrid prototype Modal dim
// (rgba(8,8,10,0.62)). No native window, so nothing dims the OS taskbar.
constexpr int kBackdropAlpha = 158; // 0.62 * 255

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

QWidget* makeMetaRow(const QString& key, const QString& value, const QString& value_object_name, QWidget* parent) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 9, 0, 9);
    layout->setSpacing(12);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(value, row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setCursor(Qt::IBeamCursor);

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    return row;
}

} // namespace

AboutOverlay::AboutOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("aboutOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    card_ = buildCard();

    // Center the card over the backdrop; the surrounding margin/stretch is the
    // dimmed, click-to-dismiss region.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->addStretch(1);
    layout->addWidget(card_, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

QFrame* AboutOverlay::buildCard() {
    auto* card = new QFrame(this);
    card->setObjectName("aboutCard");
    card->setFixedWidth(480);

    auto* main_layout = new QVBoxLayout(card);
    main_layout->setContentsMargins(28, 26, 28, 22);
    main_layout->setSpacing(0);

    const QString version = QString::fromLatin1(build::kVersion);
    const QString build_config = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
    const QString commit = QString::fromLatin1(build::kGitCommit);
    const QString author = QString::fromLatin1(kAppAuthor);

    // ── Header: aperture mark + two-tone wordmark + version line ──────────────────────────────
    auto* header_row = new QHBoxLayout();
    header_row->setContentsMargins(0, 0, 0, 0);
    header_row->setSpacing(16);

    auto* mark = new ui::brand::BrandMarkWidget(card);
    mark->setFixedSize(48, 48);
    header_row->addWidget(mark, 0, Qt::AlignVCenter);

    auto* title_col = new QVBoxLayout();
    title_col->setContentsMargins(0, 0, 0, 0);
    title_col->setSpacing(4);

    auto* wordmark = new QLabel(card);
    wordmark->setProperty("labelRole", "aboutWordmark");
    wordmark->setTextFormat(Qt::RichText);
    wordmark->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                          .arg(QString::fromLatin1(theme::ExoSnapPalette::kText0),
                               QString::fromLatin1(theme::ExoSnapPalette::kAccent)));

    auto* version_line = new QLabel(QStringLiteral("Version %1 \xc2\xb7 for Windows").arg(version), card);
    version_line->setProperty("labelRole", "aboutVersionLine");

    title_col->addWidget(wordmark);
    title_col->addWidget(version_line);
    header_row->addLayout(title_col, 1);
    main_layout->addLayout(header_row);
    main_layout->addSpacing(18);

    // ── Description ───────────────────────────────────────────────────────────────────────────
    auto* desc_label = new QLabel(QString::fromLatin1(kAppDescription), card);
    desc_label->setProperty("labelRole", "aboutDescription");
    desc_label->setWordWrap(true);
    main_layout->addWidget(desc_label);
    main_layout->addSpacing(18);

    // ── Metadata table (real build metadata only) ─────────────────────────────────────────────
    auto* meta_panel = new QFrame(card);
    meta_panel->setProperty("panelRole", "note");
    auto* meta_layout = new QVBoxLayout(meta_panel);
    meta_layout->setContentsMargins(18, 2, 18, 2);
    meta_layout->setSpacing(0);

    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("VERSION"), version, QStringLiteral("aboutValueVersion"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("BUILD"), build_config, QStringLiteral("aboutValueBuild"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("COMMIT"), commit, QStringLiteral("aboutValueCommit"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("AUTHOR"), author, QStringLiteral("aboutValueAuthor"), meta_panel));

    main_layout->addWidget(meta_panel);
    main_layout->addSpacing(22);

    // ── Actions: GitHub (configured) · Copy details · Close ───────────────────────────────────
    auto* github_btn = new QPushButton(QStringLiteral("GitHub"), card);
    github_btn->setObjectName(QStringLiteral("aboutGitHubButton"));
    github_btn->setProperty("role", "ghost");
    github_btn->setProperty("url", QString::fromLatin1(kGitHubUrl));
    github_btn->setCursor(Qt::PointingHandCursor);
    connect(github_btn, &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kGitHubUrl))); });

    auto* copy_btn = new QPushButton(QStringLiteral("Copy details"), card);
    copy_btn->setObjectName(QStringLiteral("aboutCopyButton"));
    copy_btn->setProperty("role", "ghost");
    copy_btn->setCursor(Qt::PointingHandCursor);
    connect(copy_btn, &QPushButton::clicked, this, [version, build_config, commit, author]() {
        const QString details = QStringLiteral("ExoSnap\nVersion: %1\nBuild: %2\nCommit: %3\nAuthor: %4")
                                    .arg(version, build_config, commit, author);
        QGuiApplication::clipboard()->setText(details);
    });

    auto* close_btn = new QPushButton(QStringLiteral("Close"), card);
    close_btn->setObjectName(QStringLiteral("aboutCloseButton"));
    close_btn->setProperty("role", "primary");
    close_btn->setCursor(Qt::PointingHandCursor);
    close_btn->setFixedWidth(96);
    connect(close_btn, &QPushButton::clicked, this, &AboutOverlay::closeOverlay);

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->setSpacing(10);
    btn_row->addWidget(github_btn);
    btn_row->addWidget(copy_btn);
    btn_row->addStretch(1);
    btn_row->addWidget(close_btn);
    main_layout->addLayout(btn_row);

    return card;
}

void AboutOverlay::openOverlay() {
    syncGeometryToParent();
    setVisible(true);
    raise();
    setFocus(Qt::OtherFocusReason);
}

void AboutOverlay::closeOverlay() {
    // isHidden() reflects this widget's own show/hide state regardless of whether
    // an ancestor is currently visible, so the guard is correct in tests too.
    if (isHidden())
        return;
    setVisible(false);
    emit closed();
}

bool AboutOverlay::isOpen() const noexcept {
    return !isHidden();
}

void AboutOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void AboutOverlay::mousePressEvent(QMouseEvent* event) {
    // The card is an opaque child and consumes its own clicks; any press that
    // reaches the overlay is on the dimmed backdrop, so dismiss.
    if (card_ == nullptr || !card_->geometry().contains(event->pos())) {
        closeOverlay();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void AboutOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    QColor backdrop(theme::ExoSnapPalette::kBg0);
    backdrop.setAlpha(kBackdropAlpha);
    painter.fillRect(rect(), backdrop);
}

bool AboutOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move || event->type() == QEvent::Show)) {
        syncGeometryToParent();
    }
    return QWidget::eventFilter(watched, event);
}

void AboutOverlay::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncGeometryToParent();
    raise();
    setFocus(Qt::OtherFocusReason);
}

void AboutOverlay::syncGeometryToParent() {
    if (QWidget* host = parentWidget())
        setGeometry(host->rect());
}

} // namespace exosnap::ui::dialogs
