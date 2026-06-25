#include "AboutOverlay.h"
#include "ExoSnapBuildInfo.h"
#include "UpdateSettingsPanel.h"

#include "../brand/BrandMarkWidget.h"
#include "../theme/ExoSnapPalette.h"
#include "../theme/ExoSnapTheme.h"

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
#include <QtGlobal>

namespace exosnap::ui::dialogs {
namespace {

constexpr const char* kAppAuthor = "Exoridus";
// Canonical repository URL — taken from the configured git remote (git@github.com:Exoridus/exosnap),
// not invented. The GitHub action is only shown because this URL is real.
constexpr const char* kGitHubUrl = "https://github.com/Exoridus/exosnap";
// Author profile URL — derivable from the repo owner in kGitHubUrl, not invented.
constexpr const char* kAuthorProfileUrl = "https://github.com/Exoridus";
constexpr const char* kReleasesUrl = "https://github.com/Exoridus/exosnap/releases";
constexpr const char* kAppDescription =
    "A calm, preview-first screen recorder with a high-performance GPU pipeline, multi-track audio "
    "routing, and diagnostics when you need them.";

// Default channel shown in the metadata table until MainWindow calls setChannelHint().
constexpr const char* kDefaultChannel = "Stable";

// Backdrop tint behind the card — matches the Hybrid prototype Modal dim
// (rgba(8,8,10,0.62)). No native window, so nothing dims the OS taskbar.
constexpr int kBackdropAlpha = 158; // 0.62 * 255

QFrame* makeHairline(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setProperty("frameRole", "sectionRuleLine");
    return line;
}

// url: if non-empty, the value label becomes a clickable hyperlink opening that URL.
QWidget* makeMetaRow(const QString& key, const QString& value, const QString& value_object_name, QWidget* parent,
                     const QString& url = {}) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    // #01: no fixed row height; rows get min-height 28px, padding 6px 0; container gets 4px bottom.
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(12);
    row->setMinimumHeight(28);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");

    if (!url.isEmpty()) {
        value_label->setTextFormat(Qt::RichText);
        value_label->setText(QStringLiteral("<a href='%1'>%2</a>").arg(url, value));
        value_label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        value_label->setOpenExternalLinks(true);
    } else {
        value_label->setText(value);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value_label->setCursor(Qt::IBeamCursor);
    }

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);
    return row;
}

// Like makeMetaRow but returns the value QLabel so callers can update it later.
// The outer QWidget is added to `parent_layout`; the inner QLabel is returned.
QLabel* makeMetaRowDynamic(const QString& key, const QString& initial_value, const QString& value_object_name,
                           QWidget* parent, QVBoxLayout* parent_layout) {
    auto* row = new QWidget(parent);
    row->setObjectName("aboutMetaRow");
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 6, 0, 6);
    layout->setSpacing(12);
    row->setMinimumHeight(28);

    auto* key_label = new QLabel(key, row);
    key_label->setProperty("labelRole", "aboutMetaKey");
    key_label->setFixedWidth(96);

    auto* value_label = new QLabel(initial_value, row);
    value_label->setObjectName(value_object_name);
    value_label->setProperty("labelRole", "aboutMetaValue");
    value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value_label->setCursor(Qt::IBeamCursor);

    layout->addWidget(key_label);
    layout->addWidget(value_label, 1);

    parent_layout->addWidget(row);
    return value_label;
}

} // namespace

AboutOverlay::AboutOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("aboutOverlay");
    setFocusPolicy(Qt::StrongFocus);
    setVisible(false);

    // Hidden update settings panel — kept for MainWindow wiring compatibility only.
    // MainWindow calls updatePanel()->setState/setModel/setRecordingActive; this is
    // a no-op visually since the panel is never added to any visible layout.
    update_panel_ = new UpdateSettingsPanel(this);
    update_panel_->setObjectName(QStringLiteral("aboutUpdatePanel"));
    update_panel_->hide();
    connect(update_panel_, &UpdateSettingsPanel::checkRequested, this, &AboutOverlay::checkForUpdatesRequested);

    card_ = buildCard();

    // Center the card over the backdrop.
    // The surrounding stretch is the dimmed, click-to-dismiss region.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->addStretch(1);
    layout->addWidget(card_, 0, Qt::AlignHCenter);
    layout->addStretch(1);

    if (parent != nullptr)
        parent->installEventFilter(this);
}

QFrame* AboutOverlay::buildCard() {
    const QString version = QString::fromLatin1(build::kVersion);
    const QString build_config = QString::fromLatin1(EXOSNAP_BUILD_CONFIG);
    const QString commit = QString::fromLatin1(build::kGitCommit);
    const QString author = QString::fromLatin1(kAppAuthor);
    const QString channel = QString::fromLatin1(kDefaultChannel);

    // ── About info card ──────────────────────────────────────────────────────────────
    auto* card = new QFrame(this);
    card->setObjectName("aboutCard");
    card->setFixedWidth(480);

    auto* main_layout = new QVBoxLayout(card);
    main_layout->setContentsMargins(28, 26, 28, 22);
    main_layout->setSpacing(0);

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

    wordmark_ = new QLabel(card);
    wordmark_->setProperty("labelRole", "aboutWordmark");
    wordmark_->setTextFormat(Qt::RichText);
    refreshBrand();

    auto* version_line = new QLabel(QStringLiteral("Version %1 \xc2\xb7 for Windows").arg(version), card);
    version_line->setProperty("labelRole", "aboutVersionLine");

    title_col->addWidget(wordmark_);
    title_col->addWidget(version_line);
    header_row->addLayout(title_col, 1);

    // Compact × dismiss button — top-right of the card header, replaces the large primary Close.
    auto* dismiss_btn = new QPushButton(QString::fromLatin1("\xd7"), card);
    dismiss_btn->setObjectName(QStringLiteral("aboutCloseButton"));
    dismiss_btn->setFixedSize(28, 28);
    dismiss_btn->setCursor(Qt::PointingHandCursor);
    connect(dismiss_btn, &QPushButton::clicked, this, &AboutOverlay::closeOverlay);
    header_row->addWidget(dismiss_btn, 0, Qt::AlignTop);

    main_layout->addLayout(header_row);
    main_layout->addSpacing(18);

    // ── Description ───────────────────────────────────────────────────────────────────────────
    auto* desc_label = new QLabel(QString::fromLatin1(kAppDescription), card);
    desc_label->setProperty("labelRole", "aboutDescription");
    desc_label->setWordWrap(true);
    main_layout->addWidget(desc_label);
    main_layout->addSpacing(18);

    // ── Metadata table ────────────────────────────────────────────────────────────────────────
    // Rows: Version / Build / Commit / Channel / Author  (v10: QT row removed, Channel added)
    auto* meta_panel = new QFrame(card);
    meta_panel->setProperty("panelRole", "note");
    auto* meta_layout = new QVBoxLayout(meta_panel);
    // #01: 4px bottom padding so last rule isn't flush with inset edge.
    meta_layout->setContentsMargins(18, 2, 18, 4);
    meta_layout->setSpacing(0);

    // Commit URL is constructable from the known repo URL + the real commit SHA.
    const QString commit_url = QStringLiteral("%1/commit/%2").arg(QString::fromLatin1(kGitHubUrl), commit);

    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("VERSION"), version, QStringLiteral("aboutValueVersion"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("BUILD"), build_config, QStringLiteral("aboutValueBuild"), meta_panel));
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(
        makeMetaRow(QStringLiteral("COMMIT"), commit, QStringLiteral("aboutValueCommit"), meta_panel, commit_url));
    meta_layout->addWidget(makeHairline(meta_panel));
    // Channel row — dynamic, updated via setChannelHint().
    channel_value_ = makeMetaRowDynamic(QStringLiteral("CHANNEL"), channel, QStringLiteral("aboutValueChannel"),
                                        meta_panel, meta_layout);
    meta_layout->addWidget(makeHairline(meta_panel));
    meta_layout->addWidget(makeMetaRow(QStringLiteral("AUTHOR"), author, QStringLiteral("aboutValueAuthor"), meta_panel,
                                       QString::fromLatin1(kAuthorProfileUrl)));

    main_layout->addWidget(meta_panel);
    main_layout->addSpacing(14);

    // ── Quiet update status line ──────────────────────────────────────────────────────────────
    // A single dim line; updated via setUpdateStatusText(). Hidden when empty.
    update_status_line_ = new QLabel(card);
    update_status_line_->setObjectName(QStringLiteral("aboutUpdateStatusLine"));
    update_status_line_->setProperty("labelRole", "aboutUpdateStatus");
    update_status_line_->setWordWrap(false);
    update_status_line_->setVisible(false); // hidden until setUpdateStatusText() provides a value
    main_layout->addWidget(update_status_line_);
    main_layout->addSpacing(4);

    // ── Actions: GitHub · Copy details · Release notes ────────────────────────────────────────
    // No primary Close button — dismiss via × (top-right), Escape, or backdrop click.
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
    connect(copy_btn, &QPushButton::clicked, this, [this, version, build_config, commit, author]() {
        const QString ch = channel_value_ ? channel_value_->text() : QStringLiteral("Stable");
        const QString details = QStringLiteral("ExoSnap\nVersion: %1\nBuild: %2\nCommit: %3\nChannel: %4\nAuthor: %5")
                                    .arg(version, build_config, commit, ch, author);
        QGuiApplication::clipboard()->setText(details);
    });

    auto* release_notes_btn = new QPushButton(QStringLiteral("Release notes"), card);
    release_notes_btn->setObjectName(QStringLiteral("aboutReleaseNotesButton"));
    release_notes_btn->setProperty("role", "quiet");
    release_notes_btn->setCursor(Qt::PointingHandCursor);
    connect(release_notes_btn, &QPushButton::clicked, this,
            []() { QDesktopServices::openUrl(QUrl(QString::fromLatin1(kReleasesUrl))); });

    auto* btn_row = new QHBoxLayout();
    btn_row->setContentsMargins(0, 0, 0, 0);
    btn_row->setSpacing(10);
    btn_row->addWidget(github_btn);
    btn_row->addWidget(copy_btn);
    btn_row->addWidget(release_notes_btn);
    btn_row->addStretch(1);
    main_layout->addLayout(btn_row);

    return card;
}

void AboutOverlay::setUpdateStatusText(const QString& text) {
    if (update_status_line_ == nullptr)
        return;
    if (text.isEmpty()) {
        update_status_line_->clear();
        update_status_line_->setVisible(false);
    } else {
        update_status_line_->setText(text);
        update_status_line_->setVisible(true);
    }
}

void AboutOverlay::setChannelHint(const QString& channel) {
    if (channel_value_ == nullptr || channel.isEmpty())
        return;
    channel_value_->setText(channel);
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
    QColor backdrop(QString::fromUtf8(theme::ActiveTheme().bg));
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

void AboutOverlay::refreshBrand() {
    if (wordmark_ == nullptr)
        return;
    const auto& t = theme::ActiveTheme();
    wordmark_->setText(QStringLiteral("<span style=\"color:%1;\">exo</span><span style=\"color:%2;\">snap</span>")
                           .arg(QString::fromUtf8(t.ink), QString::fromUtf8(t.ac)));
}

} // namespace exosnap::ui::dialogs
